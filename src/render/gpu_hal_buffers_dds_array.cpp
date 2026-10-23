#include <vector>
#include "gpu_hal_buffers_internal.h"
#include <monkey_dust/render/gpu_hal_free_functions.h>

// Per-mip BC-compressed byte size (4×4 texel blocks) — declared in
// gpu_hal_buffers_internal.h, shared with gpu_hal_buffers_dds.cpp's
// GpuTexture::InitFromDDS.
size_t s_bc_mip_bytes(int w, int h, int bytes_per_block)
{
    int bw = (w+3)/4, bh = (h+3)/4;
    return (size_t)bw * bh * (size_t)bytes_per_block;
}

// ── DDS (BC1/DXT1 or BC3/DXT5) 2D texture array loader ───────────────────────

// Returns height(12), width(16), mip_count(28), is_bc3/is_bc1, data_offset.
// BC1 support added for Kenshi's terrain _NML.dds files, which are DXT1 (not
// DXT5 like the _DIF.dds diffuse array) -- confirmed via FourCC byte.
static bool s_parse_dds(const uint8_t* buf, uint32_t len,
                         int& w, int& h, int& mips, bool& is_bc3, bool& is_bc1, uint32_t& data_off)
{
    if (len < 128 || buf[0]!='D'||buf[1]!='D'||buf[2]!='S'||buf[3]!=' ') return false;
    auto r32 = [&](uint32_t o) { uint32_t v; memcpy(&v, buf+o, 4); return v; };
    h       = (int)r32(12);
    w       = (int)r32(16);
    mips    = (int)r32(28); if (mips < 1) mips = 1;
    // DDPIXELFORMAT at offset 76: flags(+4), fourCC(+8)
    uint32_t pf_flags = r32(80);
    uint32_t fourcc   = r32(84);
    is_bc3   = (pf_flags & 4u) && (fourcc == 0x35545844u); // DDPF_FOURCC && 'DXT5'
    is_bc1   = (pf_flags & 4u) && (fourcc == 0x31545844u); // DDPF_FOURCC && 'DXT1'
    data_off = 128;
    return true;
}

// DXT3 / BC2 (Kenshi hair and beard textures): 16 B per block like BC3; only a single layer is supported for it (the character editor's attachments).
static bool s_is_bc2(const char* buf, uint32_t len) {
    if (!buf || len < 128) return false;
    uint32_t pf_flags, fourcc; memcpy(&pf_flags, buf + 80, 4); memcpy(&fourcc, buf + 84, 4);
    return (pf_flags & 4u) && fourcc == 0x33545844u; // 'DXT3'
}

bool GpuTexture::InitFromDDSArray(const char* const* paths, int count,
                                   const GpuSamplerDesc& /*s*/)
{
    // Note: the passed sampler desc's mip fields are intentionally not used
    // here (unlike CreateSDLSampler(), which gates max_lod on gen_mipmap) —
    // this loader always uploads a full real mip chain straight from the DDS
    // file's own mips, so the sampler below hardcodes max_lod=ref_mips-1
    // (the correct range for that chain) regardless of the caller's
    // gen_mipmap flag. Verified via GPU debug: forcing max_lod=0 here (mip0
    // only) did not change the render, confirming mip selection was never
    // the issue in this array's sampling path.
    SDL_GPUDevice* dev = md::GpuDevice::Get().SDLDevice();
    if (!dev || count < 1 || !paths) return false;

    struct LayerData { char* buf; uint32_t len; const char* path; };
    LayerData* layers = (LayerData*)malloc((size_t)count * sizeof(LayerData));
    if (!layers) return false;
    memset(layers, 0, (size_t)count * sizeof(LayerData));
    for (int i = 0; i < count; ++i) layers[i].path = paths[i];

    // Parallel file read — each layer's fs_read_alloc touches only its own
    // buffer (no GPU calls, no MdRegistry), matching JobSystem's documented
    // safe-usage contract exactly (see job_system.h). Confirmed by direct
    // measurement this loop (previously one fopen/fread/fclose per layer in
    // series, up to 125 layers) was a real contributor to startup stalls
    // alongside the JoltWorld::AddTerrainMesh fix — reading independent files
    // in parallel across JobSystem's workers cuts wall-clock read time
    // roughly by the worker count on multi-core hardware. Falls back to
    // serial reads if JobSystem has no workers (e.g. test binaries that
    // never call JobSystem::Init()).
    auto read_one = [](void* p) {
        auto* l = (LayerData*)p;
        if (l->path && strcmp(l->path, MD_DDS_STUB_1X1) == 0) {   // a built-in white 1x1 BC3 texture: a binding that has to exist but is never read
            static const uint32_t hdr[32] = { 0x20534444u, 124, 0x81007u, 1, 1, 16, 0, 1, 0,0,0,0,0,0,0,0,0,0,0, 32, 4, 0x35545844u, 0,0,0,0,0, 0x1000, 0,0,0,0 };
            l->len = 128 + 16; l->buf = (char*)malloc(l->len); memset(l->buf, 0, l->len); memcpy(l->buf, hdr, 128);
            memset(l->buf + 128, 0xFF, 16);
            return;
        }
        l->buf = md::fs_read_alloc(l->path, &l->len);
    };
    if (JobSystem::Get().NumWorkers() > 0) {
        for (int i = 0; i < count; ++i) JobSystem::Get().Submit(read_one, &layers[i]);
        JobSystem::Get().Flush();
    } else {
        for (int i = 0; i < count; ++i) read_one(&layers[i]);
    }

    int ref_w = 0, ref_h = 0, ref_mips = 0; bool ref_bc3 = false, ref_bc1 = false;
    std::vector<uint8_t> shifted((size_t)count, 0); // 1 = half-size layer stored one mip level lower (see below)
    uint32_t ref_doff = 0;
    bool ok = true;

    for (int i = 0; i < count && ok; ++i) {
        if (!layers[i].buf) {
            fprintf(stderr, "[GpuTexture] DDS array: missing (i=%d) '%s'\n", i, paths[i] ? paths[i] : "(null)");
            MD_LOG(MD_LOG_WARNING, "[GpuTexture] DDS array: missing %s", paths[i]);
            ok = false; break;
        }
        int w, h, m; bool bc3, bc1; uint32_t doff;
        if (!s_parse_dds((const uint8_t*)layers[i].buf, layers[i].len, w, h, m, bc3, bc1, doff)) {
            fprintf(stderr, "[GpuTexture] DDS array: bad header (i=%d) '%s'\n", i, paths[i]);
            MD_LOG(MD_LOG_WARNING, "[GpuTexture] DDS array: bad header %s", paths[i]);
            ok = false; break;
        }
        if (i == 0) { ref_w=w; ref_h=h; ref_mips=m; ref_bc3=bc3; ref_bc1=bc1; ref_doff=doff; }
        else if (w*2==ref_w && h*2==ref_h && m==ref_mips-1 && bc3==ref_bc3 && bc1==ref_bc1) {
            // A half-size texture (Kenshi mixes 2048 and 1024 ground textures): its own mip chain is stored one level lower, file level k in
            // array level k+1, so the GPU's lod-1 pick reads the original pixels (same texel density per screen pixel). Only the closest
            // view (array level 0, a 2x2 tiling of the file's level 0, bytes copied unchanged) differs from Kenshi.
            shifted[i] = 1;
        }
        else if (w!=ref_w||h!=ref_h||m!=ref_mips||bc3!=ref_bc3||bc1!=ref_bc1) {
            fprintf(stderr, "[GpuTexture] DDS array: size mismatch (i=%d) '%s' w=%d h=%d m=%d bc3=%d bc1=%d vs ref w=%d h=%d m=%d bc3=%d bc1=%d\n",
                    i, paths[i], w, h, m, bc3, bc1, ref_w, ref_h, ref_mips, ref_bc3, ref_bc1);
            MD_LOG(MD_LOG_WARNING, "[GpuTexture] DDS array: layer %d size mismatch", i);
            ok = false; break;
        }
    }
    const bool ref_bc2 = ok && count == 1 && s_is_bc2(layers[0].buf, layers[0].len);
    if (ok && !ref_bc3 && !ref_bc1 && !ref_bc2) {
        MD_LOG(MD_LOG_WARNING, "[GpuTexture] DDS array: only BC1/DXT1, BC2/DXT3 (one layer) or BC3/DXT5 supported");
        ok = false;
    }

    if (!ok) {
        for (int i=0;i<count;++i) md::fs_free(layers[i].buf);
        free(layers); return false;
    }

    // Compute per-mip byte sizes and total transfer size
    const int block_bytes = (ref_bc3 || ref_bc2) ? 16 : 8;  // BC3 / BC2 = 16B/block, BC1=8B/block
    size_t mip_sz[32] = {};
    size_t layer_stride = 0;
    { int mw=ref_w, mh=ref_h;
      for (int m=0; m<ref_mips && m<32; ++m) {
          mip_sz[m] = s_bc_mip_bytes(mw, mh, block_bytes);
          layer_stride += mip_sz[m];
          mw = mw>1?mw/2:1; mh = mh>1?mh/2:1;
      }
    }
    uint32_t total = (uint32_t)(layer_stride * (size_t)count);

    SDL_GPUTextureCreateInfo ti = {};
    ti.type                 = SDL_GPU_TEXTURETYPE_2D_ARRAY;
    ti.format               = ref_bc2 ? SDL_GPU_TEXTUREFORMAT_BC2_RGBA_UNORM
                              : ref_bc3 ? SDL_GPU_TEXTUREFORMAT_BC3_RGBA_UNORM
                                        : SDL_GPU_TEXTUREFORMAT_BC1_RGBA_UNORM;
    ti.usage                = SDL_GPU_TEXTUREUSAGE_SAMPLER;
    ti.width                = (Uint32)ref_w;
    ti.height                = (Uint32)ref_h;
    ti.layer_count_or_depth = (Uint32)count;
    ti.num_levels           = (Uint32)ref_mips;
    fprintf(stderr, "[DDS array] creating texture %dx%d %d layers %d mips, transfer=%u bytes\n",
            ref_w, ref_h, count, ref_mips, total);
    sdl_tex_ = SDL_CreateGPUTexture(dev, &ti);
    if (!sdl_tex_) {
        MD_LOG(MD_LOG_WARNING, "[GpuTexture] DDS array: SDL_CreateGPUTexture failed: %s", SDL_GetError());
        for (int i=0;i<count;++i) md::fs_free(layers[i].buf);
        free(layers); return false;
    }
    md::GpuResourceTracker::Get().OnTextureCreate();
    fprintf(stderr, "[DDS array] texture created OK\n");

    SDL_GPUTransferBufferCreateInfo tbci = {};
    tbci.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    tbci.size  = total;
    SDL_GPUTransferBuffer* tb = SDL_CreateGPUTransferBuffer(dev, &tbci);
    if (!tb) {
        SDL_ReleaseGPUTexture(dev, sdl_tex_); sdl_tex_ = nullptr;
        md::GpuResourceTracker::Get().OnTextureDestroy();
        for (int i=0;i<count;++i) md::fs_free(layers[i].buf);
        free(layers); return false;
    }
    fprintf(stderr, "[DDS array] transfer buffer created OK\n");

    uint8_t* mapped = (uint8_t*)GpuMapTransfer(tb, false);
    if (mapped) {
        struct CopyJob { uint8_t* dst; const uint8_t* src; size_t layer_stride; size_t mip0; size_t bw, bh; size_t row_bytes; bool shifted; };
        std::vector<CopyJob> jobs((size_t)count);
        for (int i=0;i<count;++i) {
            const size_t bw = (size_t)((ref_w / 2 + 3) / 4), bh = (size_t)((ref_h / 2 + 3) / 4); // source level-0 size in blocks of a half-size layer
            jobs[(size_t)i] = { mapped + (size_t)i * layer_stride, (const uint8_t*)layers[i].buf + ref_doff, layer_stride, mip_sz[0], bw, bh, bw * (size_t)block_bytes, shifted[i] != 0 };
        }
        auto copy_one = [](void* p) {
            CopyJob& j = *(CopyJob*)p;
            if (!j.shifted) { memcpy(j.dst, j.src, j.layer_stride); return; }
            // level 0 of the array layer: the file's level 0 tiled 2x2 (block rows/columns copied as they are); levels 1.. = the file's levels 0..
            for (size_t r = 0; r < j.bh * 2; ++r) {
                const uint8_t* srow = j.src + (r % j.bh) * j.row_bytes;
                memcpy(j.dst + r * j.row_bytes * 2, srow, j.row_bytes);
                memcpy(j.dst + r * j.row_bytes * 2 + j.row_bytes, srow, j.row_bytes);
            }
            memcpy(j.dst + j.mip0, j.src, j.layer_stride - j.mip0);
        };
        if (JobSystem::Get().NumWorkers() > 0 && count > 1) {   // layers are disjoint ranges of the mapped buffer
            for (int i = 0; i < count; ++i) JobSystem::Get().Submit(copy_one, &jobs[(size_t)i]);
            JobSystem::Get().Flush();
        } else {
            for (int i = 0; i < count; ++i) copy_one(&jobs[(size_t)i]);
        }
        GpuUnmapTransfer(tb);
    }
    fprintf(stderr, "[DDS array] data mapped+copied OK\n");

    SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(dev);
    if (!cmd) {
        fprintf(stderr, "[DDS array] AcquireGPUCommandBuffer returned NULL!\n");
        SDL_ReleaseGPUTransferBuffer(dev, tb);
        SDL_ReleaseGPUTexture(dev, sdl_tex_); sdl_tex_ = nullptr;
        md::GpuResourceTracker::Get().OnTextureDestroy();
        for (int i=0;i<count;++i) md::fs_free(layers[i].buf);
        free(layers); return false;
    }
    fprintf(stderr, "[DDS array] cmd acquired OK\n");
    SDL_GPUCopyPass* cp = SDL_BeginGPUCopyPass(cmd);

    uint32_t tb_off = 0;
    for (int i=0;i<count;++i) {
        int mw=ref_w, mh=ref_h;
        for (int m=0; m<ref_mips && m<32; ++m) {
            SDL_GPUTextureTransferInfo src = {};
            src.transfer_buffer = tb;
            src.offset          = tb_off;
            // See InitFromDDS's identical comment above — BC1/BC3's 4x4 block
            // extent requires bufferRowLength/bufferImageHeight rounded up to
            // a multiple of 4 for small mips, independent of dst.w/dst.h.
            src.pixels_per_row  = (Uint32)((mw + 3) & ~3);
            src.rows_per_layer  = (Uint32)((mh + 3) & ~3);
            SDL_GPUTextureRegion dst = {};
            dst.texture   = sdl_tex_;
            dst.mip_level = (Uint32)m;
            dst.layer     = (Uint32)i;
            dst.w         = (Uint32)mw;
            dst.h         = (Uint32)mh;
            dst.d         = 1;
            SDL_UploadToGPUTexture(cp, &src, &dst, false);
            tb_off += (uint32_t)mip_sz[m];
            mw = mw>1?mw/2:1; mh = mh>1?mh/2:1;
        }
    }

    SDL_EndGPUCopyPass(cp);
    // Bug fix (2026-08-09): was a fire-and-forget SDL_SubmitGPUCommandBuffer
    // -- callers (TerrainRenderer::InitGroundTextureArray, off the main
    // thread) treated this function returning as "the texture is ready to
    // sample", but that only meant the upload was SUBMITTED, not that the
    // GPU had actually finished writing all layers/mips of this often-
    // hundreds-of-MB array. A consumer that starts sampling this texture
    // very soon after (terrain-vt's page-fill compute pass, which begins
    // dispatching the moment s_master_ready flips true) could read
    // partially-written/undefined texture memory -- real user report:
    // sharp-edged, plausible-but-wrong-colored rectangular blocks baked
    // permanently into a VT atlas slot, non-deterministic (depends on GPU
    // upload-vs-first-sample timing), self-healing once that slot is later
    // refilled after the real upload has long since finished. This function
    // is only ever called from a background loader thread (see this file's
    // own doc comment on the parallel file-read step) as a one-time,
    // blocking asset load -- waiting for a real fence here costs nothing a
    // caller wasn't already implicitly assuming, and makes "this function
    // returned" an honest guarantee instead of a race.
    SDL_GPUFence* fence = SDL_SubmitGPUCommandBufferAndAcquireFence(cmd);
    if (fence) {
        SDL_WaitForGPUFences(dev, true, &fence, 1);
        SDL_ReleaseGPUFence(dev, fence);
    }
    SDL_ReleaseGPUTransferBuffer(dev, tb);

    SDL_GPUSamplerCreateInfo si = {};
    si.min_filter     = SDL_GPU_FILTER_LINEAR;
    si.mag_filter     = SDL_GPU_FILTER_LINEAR;
    si.mipmap_mode    = SDL_GPU_SAMPLERMIPMAPMODE_LINEAR;
    si.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_REPEAT;
    si.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_REPEAT;
    si.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_REPEAT;
    si.min_lod        = 0.f;
    si.max_lod        = (float)(ref_mips - 1);
    sdl_sampler_ = SDL_CreateGPUSampler(dev, &si);

    w_ = ref_w; h_ = ref_h;
    for (int i=0;i<count;++i) md::fs_free(layers[i].buf);
    free(layers);

    if (!sdl_sampler_) {
        SDL_ReleaseGPUTexture(dev, sdl_tex_); sdl_tex_ = nullptr;
        md::GpuResourceTracker::Get().OnTextureDestroy();
        return false;
    }
    return true;
}

// DDS cube map: the file holds the 6 faces one after the other (+X -X +Y -Y +Z -Z), each with its whole mip chain: the layout of a 6-layer array.
bool GpuTexture::InitFromDDSCube(const char* path)
{
    SDL_GPUDevice* dev = md::GpuDevice::Get().SDLDevice();
    if (!dev || !path) return false;
    uint32_t len = 0;
    char* buf = md::fs_read_alloc(path, &len);
    if (!buf) { MD_LOG(MD_LOG_WARNING, "[GpuTexture] DDS cube: missing %s", path); return false; }
    int w, h, mips; bool bc3, bc1; uint32_t doff;
    uint32_t caps2 = 0; if (len >= 116) memcpy(&caps2, buf + 112, 4);
    if (!s_parse_dds((const uint8_t*)buf, len, w, h, mips, bc3, bc1, doff) || (!bc3 && !bc1) || (caps2 & 0xFE00u) != 0xFE00u) {
        MD_LOG(MD_LOG_WARNING, "[GpuTexture] DDS cube: %s is not a BC1/BC3 cube map with 6 faces", path);
        md::fs_free(buf); return false;
    }
    const int block_bytes = bc3 ? 16 : 8;
    size_t mip_sz[32] = {}, face_stride = 0;
    { int mw = w, mh = h; for (int m = 0; m < mips && m < 32; ++m) { mip_sz[m] = s_bc_mip_bytes(mw, mh, block_bytes); face_stride += mip_sz[m]; mw = mw > 1 ? mw / 2 : 1; mh = mh > 1 ? mh / 2 : 1; } }
    const size_t total = face_stride * 6;
    if ((size_t)doff + total > len) { MD_LOG(MD_LOG_WARNING, "[GpuTexture] DDS cube: %s is truncated", path); md::fs_free(buf); return false; }
    SDL_GPUTextureCreateInfo ti = {};
    ti.type = SDL_GPU_TEXTURETYPE_CUBE;
    ti.format = bc3 ? SDL_GPU_TEXTUREFORMAT_BC3_RGBA_UNORM : SDL_GPU_TEXTUREFORMAT_BC1_RGBA_UNORM;
    ti.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
    ti.width = (Uint32)w; ti.height = (Uint32)h; ti.layer_count_or_depth = 6; ti.num_levels = (Uint32)mips;
    sdl_tex_ = SDL_CreateGPUTexture(dev, &ti);
    if (!sdl_tex_) { MD_LOG(MD_LOG_WARNING, "[GpuTexture] DDS cube: SDL_CreateGPUTexture failed: %s", SDL_GetError()); md::fs_free(buf); return false; }
    md::GpuResourceTracker::Get().OnTextureCreate();
    SDL_GPUTransferBufferCreateInfo tbci = {};
    tbci.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD; tbci.size = (Uint32)total;
    SDL_GPUTransferBuffer* tb = SDL_CreateGPUTransferBuffer(dev, &tbci);
    uint8_t* mapped = tb ? (uint8_t*)GpuMapTransfer(tb, false) : nullptr;
    if (!mapped) {
        if (tb) SDL_ReleaseGPUTransferBuffer(dev, tb);
        SDL_ReleaseGPUTexture(dev, sdl_tex_); sdl_tex_ = nullptr; md::GpuResourceTracker::Get().OnTextureDestroy(); md::fs_free(buf); return false;
    }
    memcpy(mapped, (const uint8_t*)buf + doff, total);
    GpuUnmapTransfer(tb);
    SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(dev);
    SDL_GPUCopyPass* cp = SDL_BeginGPUCopyPass(cmd);
    uint32_t off = 0;
    for (int f = 0; f < 6; ++f) {
        int mw = w, mh = h;
        for (int m = 0; m < mips && m < 32; ++m) {
            SDL_GPUTextureTransferInfo src = {};
            src.transfer_buffer = tb; src.offset = off;
            src.pixels_per_row = (Uint32)((mw + 3) & ~3); src.rows_per_layer = (Uint32)((mh + 3) & ~3);
            SDL_GPUTextureRegion dst = {};
            dst.texture = sdl_tex_; dst.mip_level = (Uint32)m; dst.layer = (Uint32)f; dst.w = (Uint32)mw; dst.h = (Uint32)mh; dst.d = 1;
            SDL_UploadToGPUTexture(cp, &src, &dst, false);
            off += (uint32_t)mip_sz[m];
            mw = mw > 1 ? mw / 2 : 1; mh = mh > 1 ? mh / 2 : 1;
        }
    }
    SDL_EndGPUCopyPass(cp);
    SDL_GPUFence* fence = SDL_SubmitGPUCommandBufferAndAcquireFence(cmd);
    if (fence) { SDL_WaitForGPUFences(dev, true, &fence, 1); SDL_ReleaseGPUFence(dev, fence); }
    SDL_ReleaseGPUTransferBuffer(dev, tb);
    SDL_GPUSamplerCreateInfo si = {};
    si.min_filter = si.mag_filter = SDL_GPU_FILTER_LINEAR;
    si.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_LINEAR;
    si.address_mode_u = si.address_mode_v = si.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    si.min_lod = 0.f; si.max_lod = (float)(mips - 1);
    sdl_sampler_ = SDL_CreateGPUSampler(dev, &si);
    w_ = w; h_ = h;
    md::fs_free(buf);
    if (!sdl_sampler_) { SDL_ReleaseGPUTexture(dev, sdl_tex_); sdl_tex_ = nullptr; md::GpuResourceTracker::Get().OnTextureDestroy(); return false; }
    return true;
}

void GpuTexture::Shutdown() {
    SDL_GPUDevice* dev = md::GpuDevice::Get().SDLDevice();
    if (sdl_sampler_) { SDL_ReleaseGPUSampler(dev, sdl_sampler_); sdl_sampler_ = nullptr; }
    if (sdl_tex_) {
        SDL_ReleaseGPUTexture(dev, sdl_tex_);
        sdl_tex_ = nullptr;
        md::GpuResourceTracker::Get().OnTextureDestroy();
    }
    w_ = h_ = 0;
}

void GpuTexture::Bind(uint32_t unit) const {
    (void)unit;
}

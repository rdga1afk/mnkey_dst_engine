#include "gpu_hal_buffers_internal.h"
#include <monkey_dust/render/gpu_hal_free_functions.h>

// Single-texture DDS loader (2D, not array) — see gpu_hal.h's InitFromDDS
// doc comment. Supports two pixelformats: uncompressed DDPF_RGB 32bpp
// (tools/md_stitch_terrain.py's earlier format, single fread, GPU-generated
// mipmaps) and BC3/DXT5 (current format — see md_stitch_terrain.py's own
// doc comment for why: 4:1 size vs uncompressed, GPU-native decode at
// sample time, same rationale as the existing BC1/BC3 ground-texture DDS
// arrays this codebase already uses via InitFromDDSArray). BC3 files carry
// their own pre-baked mip chain (BC-compressed textures can't be
// GPU-auto-mipmapped the way uncompressed ones can), read and uploaded
// level-by-level below — reuses s_bc_mip_bytes, the same per-mip-size
// helper InitFromDDSArray already uses.
bool GpuTexture::InitFromDDS(const char* path, const GpuSamplerDesc& s) {
    FILE* f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "[GpuTexture] dds open failed: %s\n", path); return false; }

    uint8_t header[128];
    if (fread(header, 1, 128, f) != 128 ||
        header[0] != 'D' || header[1] != 'D' || header[2] != 'S' || header[3] != ' ') {
        fprintf(stderr, "[GpuTexture] dds bad magic: %s\n", path);
        fclose(f);
        return false;
    }
    auto r32 = [&](uint32_t o) { uint32_t v; memcpy(&v, header + o, 4); return v; };
    int w = (int)r32(16), h = (int)r32(12);
    int mips = (int)r32(28); if (mips < 1) mips = 1;
    uint32_t pf_flags = r32(80);
    uint32_t fourcc    = r32(84);
    uint32_t rgb_bit_count = r32(88);
    static constexpr uint32_t DDPF_RGB    = 0x40;
    static constexpr uint32_t DDPF_FOURCC = 0x4;
    static constexpr uint32_t FOURCC_DXT5 = 0x35545844u;
    bool is_rgb32 = (pf_flags & DDPF_RGB) && rgb_bit_count == 32;
    bool is_bc3   = (pf_flags & DDPF_FOURCC) && fourcc == FOURCC_DXT5;
    if ((!is_rgb32 && !is_bc3) || w <= 0 || h <= 0) {
        fprintf(stderr, "[GpuTexture] dds unsupported (need uncompressed 32bpp DDPF_RGB or "
                "BC3/DXT5) %dx%d flags=%#x fourcc=%#x bitcount=%u: %s\n",
                w, h, pf_flags, fourcc, rgb_bit_count, path);
        fclose(f);
        return false;
    }

    if (is_bc3) {
        size_t mip_sz[32] = {};
        size_t total = 0;
        { int mw = w, mh = h;
          for (int m = 0; m < mips && m < 32; ++m) {
              mip_sz[m] = s_bc_mip_bytes(mw, mh, 16);
              total += mip_sz[m];
              mw = mw > 1 ? mw / 2 : 1; mh = mh > 1 ? mh / 2 : 1;
          }
        }
        uint8_t* data = (uint8_t*)malloc(total);
        bool read_ok = data && fread(data, 1, total, f) == total;
        fclose(f);
        if (!read_ok) {
            fprintf(stderr, "[GpuTexture] dds bc3 short read: %s\n", path);
            free(data);
            return false;
        }

        SDL_GPUDevice* dev = md::GpuDevice::Get().SDLDevice();
        if (!dev) { free(data); return false; }

        SDL_GPUTextureCreateInfo ti = {};
        ti.type                 = SDL_GPU_TEXTURETYPE_2D;
        ti.format               = SDL_GPU_TEXTUREFORMAT_BC3_RGBA_UNORM;
        ti.usage                = SDL_GPU_TEXTUREUSAGE_SAMPLER;
        ti.width                = (Uint32)w;
        ti.height               = (Uint32)h;
        ti.layer_count_or_depth = 1;
        ti.num_levels           = (Uint32)mips;
        sdl_tex_ = SDL_CreateGPUTexture(dev, &ti);
        if (!sdl_tex_) {
            MD_LOG(MD_LOG_WARNING, "[GpuTexture] dds bc3 SDL_CreateGPUTexture failed: %s", SDL_GetError());
            free(data);
            return false;
        }
        md::GpuResourceTracker::Get().OnTextureCreate();

        SDL_GPUTransferBufferCreateInfo tbci = {};
        tbci.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
        tbci.size  = (Uint32)total;
        SDL_GPUTransferBuffer* tb = SDL_CreateGPUTransferBuffer(dev, &tbci);
        if (!tb) {
            SDL_ReleaseGPUTexture(dev, sdl_tex_); sdl_tex_ = nullptr;
            md::GpuResourceTracker::Get().OnTextureDestroy();
            free(data);
            return false;
        }
        uint8_t* mapped = (uint8_t*)GpuMapTransfer(tb, false);
        if (mapped) { memcpy(mapped, data, total); GpuUnmapTransfer(tb); }
        free(data);

        SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(dev);
        if (!cmd) {
            SDL_ReleaseGPUTransferBuffer(dev, tb);
            SDL_ReleaseGPUTexture(dev, sdl_tex_); sdl_tex_ = nullptr;
            md::GpuResourceTracker::Get().OnTextureDestroy();
            return false;
        }
        SDL_GPUCopyPass* cp = SDL_BeginGPUCopyPass(cmd);
        uint32_t tb_off = 0;
        { int mw = w, mh = h;
          for (int m = 0; m < mips && m < 32; ++m) {
              SDL_GPUTextureTransferInfo src = {};
              src.transfer_buffer = tb;
              src.offset          = tb_off;
              // BC3's 4x4 texel block extent: bufferRowLength/bufferImageHeight
              // must be a multiple of 4 even when a small mip's own width/height
              // (dst.w/dst.h below) drops under 4 (VUID-vkCmdCopyBufferToImage-
              // bufferRowLength/bufferImageHeight-09106/09107) — s_bc_mip_bytes
              // already rounds the byte size up the same way, this just makes
              // the transfer-layout fields agree with that rounding.
              src.pixels_per_row  = (Uint32)((mw + 3) & ~3);
              src.rows_per_layer  = (Uint32)((mh + 3) & ~3);
              SDL_GPUTextureRegion dst = {};
              dst.texture   = sdl_tex_;
              dst.mip_level = (Uint32)m;
              dst.layer     = 0;
              dst.w = (Uint32)mw; dst.h = (Uint32)mh; dst.d = 1;
              SDL_UploadToGPUTexture(cp, &src, &dst, false);
              tb_off += (uint32_t)mip_sz[m];
              mw = mw > 1 ? mw / 2 : 1; mh = mh > 1 ? mh / 2 : 1;
          }
        }
        SDL_EndGPUCopyPass(cp);
        SDL_SubmitGPUCommandBuffer(cmd);
        SDL_ReleaseGPUTransferBuffer(dev, tb);

        sdl_sampler_ = CreateSDLSampler(dev, s);
        w_ = w; h_ = h;
        if (!sdl_sampler_) {
            SDL_ReleaseGPUTexture(dev, sdl_tex_); sdl_tex_ = nullptr;
            md::GpuResourceTracker::Get().OnTextureDestroy();
            return false;
        }
        return true;
    }

    size_t row_bytes = (size_t)w * 4;
    size_t total_bytes = row_bytes * (size_t)h;
    uint8_t* rgba = (uint8_t*)malloc(total_bytes);
    if (!rgba) { fclose(f); return false; }

    bool read_ok;
    if (!s.flip_v) {
        read_ok = fread(rgba, 1, total_bytes, f) == total_bytes;
    } else {
        read_ok = true;
        for (int row = 0; row < h && read_ok; ++row) {
            uint8_t* dst = rgba + (size_t)(h - 1 - row) * row_bytes;
            read_ok = fread(dst, 1, row_bytes, f) == row_bytes;
        }
    }
    fclose(f);
    if (!read_ok) {
        fprintf(stderr, "[GpuTexture] dds short read: %s\n", path);
        free(rgba);
        return false;
    }

    w_ = w; h_ = h;
    bool ok = InitFromMemory(rgba, w, h, s);
    free(rgba);
    return ok;
}

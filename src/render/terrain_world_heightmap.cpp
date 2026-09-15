#include <monkey_dust/render/terrain_world_heightmap.h>
#ifdef MD_SDL_GPU
#include <monkey_dust/render/gpu_compute.h>
#include <monkey_dust/render/gpu_copy_pass.h>
#include <monkey_dust/render/gpu_sampler_texture.h>
#include <monkey_dust/render/gpu_hal_free_functions.h>
#include <monkey_dust/render/gpu_device.h>
#include <monkey_dust/world/terrain_gen.h>
#include <monkey_dust/world/chunk_def.h>
#include <monkey_dust/world/terrain_chunk.h>  // TERRAIN_GRID
#include <cstdio>
#include <cstdlib>
#include <cmath>

namespace {
// Field order matches shaders/terrain_worldmap_normal_bake.comp's
// NormalBakeUBO exactly (std140).
struct NormalBakeUBO {
    float world_params[4]; // x=worldExtent_m, y=resTexels, z=heightRange_m, w=heightMin_m
};
// Kenshi's real world is a fixed 64x64 zone grid (documented in
// terrain_gen.h's own "Terrain Atlas API" comment) -- not derived from
// any runtime state, safe to hardcode same as that header already does.
constexpr int kWorldZones = 64;

// Maps a world-grid sample index (0..kWorldZones*TERRAIN_GRID) to (zone
// index, local col/row 0..TERRAIN_GRID) -- zones share their boundary
// vertex (zone Z's col=TERRAIN_GRID == zone Z+1's col=0), so only the
// very last global sample needs the "clamp into the last zone's last
// col" special case. Same convention TerrainQuadtreeRenderer::
// s_sample_and_pack (terrain_quadtree_renderer.cpp) already uses for a
// windowed region, generalised here to the whole world at once.
void SampleIndexToZone(int gi, int& zi, int& local) {
    zi    = gi / TERRAIN_GRID;
    local = gi % TERRAIN_GRID;
    if (zi >= kWorldZones) { zi = kWorldZones - 1; local = TERRAIN_GRID; }
}
} // namespace

bool TerrainWorldHeightmap::Init(md::GpuDeviceHandle dev) {
    if (!TerrainAtlas_Loaded()) {
        fprintf(stderr, "[TerrainWorldHeightmap] TerrainAtlas not loaded\n");
        return false;
    }

    const int N = kWorldZones * TERRAIN_GRID + 1; // 8193
    world_extent_ = (float)kWorldZones * CHUNK_SIZE;
    res_ = N;

    // Transient CPU buffers -- freed after upload, not stored on the
    // instance (unlike the old windowed system, this only ever runs
    // once, so there is no need to keep scratch storage resident).
    float* h_tmp = (float*)malloc(sizeof(float) * (size_t)N * (size_t)N);
    if (!h_tmp) {
        fprintf(stderr, "[TerrainWorldHeightmap] h_tmp alloc failed (%dx%d)\n", N, N);
        return false;
    }

    float hmin = 1e9f, hmax = -1e9f;
    for (int row = 0; row < N; ++row) {
        int zzi, lrow; SampleIndexToZone(row, zzi, lrow);
        for (int col = 0; col < N; ++col) {
            int zxi, lcol; SampleIndexToZone(col, zxi, lcol);
            float h = TerrainAtlas_GetHeight(zxi, zzi, lcol, lrow);
            h_tmp[(size_t)row * N + col] = h;
            if (h < hmin) hmin = h;
            if (h > hmax) hmax = h;
        }
    }
    float range = hmax - hmin;
    if (range < 1.f) range = 1.f;
    height_min_ = hmin;
    height_max_ = hmin + range;

    // Pack as raw uint16 -- R16_UNORM, direct/lossless, no byte-split
    // packing (see header doc comment for why this differs from the old
    // windowed system's RGBA8 R+G-byte hack).
    uint16_t* h16 = (uint16_t*)malloc(sizeof(uint16_t) * (size_t)N * (size_t)N);
    if (!h16) {
        free(h_tmp);
        fprintf(stderr, "[TerrainWorldHeightmap] h16 alloc failed (%dx%d)\n", N, N);
        return false;
    }
    for (size_t i = 0; i < (size_t)N * (size_t)N; ++i) {
        float norm = (h_tmp[i] - hmin) / range;
        uint32_t v = (uint32_t)(norm * 65535.f + 0.5f);
        if (v > 65535u) v = 65535u;
        h16[i] = (uint16_t)v;
    }
    free(h_tmp);

    // #398 mip-chain removal (2026-08-23): this texture is ALWAYS sampled
    // at explicit textureLod(..., 0.0) in the live vertex shader path
    // (terrain_quadtree.vert's SampleHeightFrac/SampleNormal) -- geomorph's
    // "coarse" sample is a resample at a DIFFERENT WORLD POSITION on this
    // same LOD 0, not a different mip level. The one reader that COULD use
    // mip>0 (TerrainVtPageCache's page-fill compute, via TH_SampleHeightFrac's
    // explicit lod param) was provably dead in every exercised path (VT
    // cache disabled 2026-08-09) and the class itself was deleted entirely
    // in БОРГ-TERRAIN-2 (2026-09-13). Full mip chain was previously
    // floor(log2(N))+1 (14 levels for N=8193) -- ~44MB of never-sampled
    // GPU memory. If VT paging is ever revived, restore that formula here.
    const uint32_t kNumLevels = 1;
    GpuSamplerDesc tex_sdesc;
    tex_sdesc.min_filter = GpuSamplerDesc::Filter::LINEAR; // mipmap_mode moot with 1 level
    tex_sdesc.mag_filter = GpuSamplerDesc::Filter::LINEAR;
    tex_sdesc.wrap_s     = GpuSamplerDesc::Wrap::CLAMP_TO_EDGE;
    tex_sdesc.wrap_t     = GpuSamplerDesc::Wrap::CLAMP_TO_EDGE;
    tex_sdesc.gen_mipmap = false; // -> CreateSDLSampler's max_lod=0, matches kNumLevels=1
    // COLOR_TARGET usage (added internally by InitRenderTarget) is required
    // by SDL_GenerateMipmapsForGPUTexture even though this texture is never
    // bound as an actual pipeline color attachment anywhere -- matches
    // GpuTexture::InitFromMemory's own gen_mipmap path, same requirement.
    if (!tex_.InitRenderTarget(N, N, tex_sdesc, SDL_GPU_TEXTUREFORMAT_R16_UNORM)) {
        fprintf(stderr, "[TerrainWorldHeightmap] SDL_CreateGPUTexture(R16_UNORM, %dx%d) failed: %s\n",
                N, N, SDL_GetError());
        free(h16);
        return false;
    }
    if (!tex_.SDLSampler()) {
        fprintf(stderr, "[TerrainWorldHeightmap] sampler create failed: %s\n", SDL_GetError());
        free(h16);
        tex_.Shutdown();
        return false;
    }

    SDL_GPUTransferBufferCreateInfo tbi{};
    tbi.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    tbi.size  = (Uint32)((size_t)N * (size_t)N * sizeof(uint16_t));
    SDL_GPUTransferBuffer* tb = GpuCreateTransferBuffer(dev, &tbi);
    if (!tb) {
        fprintf(stderr, "[TerrainWorldHeightmap] transfer buffer create failed: %s\n", SDL_GetError());
        free(h16);
        tex_.Shutdown();
        return false;
    }
    void* ptr = GpuMapTransfer(tb, false);
    if (ptr) memcpy(ptr, h16, (size_t)N * (size_t)N * sizeof(uint16_t));
    GpuUnmapTransfer(tb);
    free(h16);

    md::GpuCommandBufferHandle cmd = md::GpuDevice::Get().AcquireCommandBuffer();
    GpuCopyPass cp;
    cp.Begin(cmd);
    SDL_GPUTextureTransferInfo src{};
    src.transfer_buffer = tb;
    src.pixels_per_row   = (Uint32)N;
    src.rows_per_layer   = (Uint32)N;
    SDL_GPUTextureRegion dst{};
    dst.texture = tex_.SDLTexture();
    dst.w = (Uint32)N; dst.h = (Uint32)N; dst.d = 1;
    cp.UploadTexture(src, dst, false);
    cp.End();
    // #398: no SDL_GenerateMipmapsForGPUTexture call -- single level only,
    // see kNumLevels=1's doc comment above.
    md::GpuDevice::Get().Submit(cmd);
    GpuReleaseTransferBuffer(dev, tb);

    fprintf(stderr, "[TerrainWorldHeightmap] ready: %dx%d R16_UNORM, %u mips, height=[%.2f,%.2f]m, extent=%.1fm\n",
            N, N, kNumLevels, height_min_, height_max_, world_extent_);

    // Full-variant Phase 3 (serene-pondering-teapot.md): bake the
    // world-wide normal texture now, once, right after the height texture
    // is ready -- same "one-time startup cost" class as everything above,
    // not a per-frame or per-window operation.
    //
    // 2026-08-29 (task #556 falsification test, docs/research/
    // OGRE_NEXT_TERRA_NORMAL_MIP_DEEPSEEK_RESEARCH.md): this texture used
    // to be num_levels=1, always sampled at textureLod(...,0.0) regardless
    // of the sampling vertex's mesh LOD tier -- a coarse/distant node's
    // 17x17 grid vertices are spaced many native normal-texels apart, so
    // each one point-samples ONE native texel of a high-frequency signal
    // instead of an average over its true world-space footprint. On steep
    // Kenshi cliffs (large per-texel normal variance) that is classic
    // undersampling -> per-vertex normal noise -> the reported black/white
    // "cliff speckle" (tests/editor_scenarios/editor_verify_bw_pattern_*.lua).
    // Fix: give normal_tex_ a real mip chain (same formula as this file's
    // OLD pre-#398 height mip count) and have terrain_quadtree.vert fetch
    // it at a LOD derived from the vertex's own texelSize vs this texture's
    // native texel size, so a coarse node's vertices sample a pre-filtered
    // (box-averaged), not raw, normal.
    Uint32 normal_num_levels = 1;
    { int sz = N; while (sz > 1) { sz >>= 1; ++normal_num_levels; } }
    {
        GpuSamplerDesc normal_sdesc;
        normal_sdesc.min_filter = GpuSamplerDesc::Filter::LINEAR_MIPMAP; // -> mipmap_mode=LINEAR
        normal_sdesc.mag_filter = GpuSamplerDesc::Filter::LINEAR;
        normal_sdesc.wrap_s     = GpuSamplerDesc::Wrap::CLAMP_TO_EDGE;
        normal_sdesc.wrap_t     = GpuSamplerDesc::Wrap::CLAMP_TO_EDGE;
        normal_sdesc.gen_mipmap = true; // -> CreateSDLSampler's max_lod=1000, clamps to normal_num_levels-1 anyway
        const SDL_GPUTextureUsageFlags normal_usage =
            SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_WRITE
            | SDL_GPU_TEXTUREUSAGE_SAMPLER
            | SDL_GPU_TEXTUREUSAGE_COLOR_TARGET; // required by SDL_GenerateMipmapsForGPUTexture below, same as tex_'s own (now-unused) mip path
        if (!normal_tex_.InitCompute(N, N, SDL_GPU_TEXTUREFORMAT_R8G8_SNORM,
                                      normal_usage, normal_num_levels, normal_sdesc)) {
            fprintf(stderr, "[TerrainWorldHeightmap] normal texture create failed: %s\n", SDL_GetError());
            tex_.Shutdown();
            return false;
        }
        if (!normal_tex_.SDLSampler()) {
            fprintf(stderr, "[TerrainWorldHeightmap] normal sampler create failed: %s\n", SDL_GetError());
            normal_tex_.Shutdown();
            tex_.Shutdown();
            return false;
        }

        GpuComputePipeline bake_pipeline;
        GpuComputePipeline::Desc pd{};
        pd.glsl_path           = "shaders/terrain_worldmap_normal_bake.comp";
        pd.num_uniform_buffers = 1;
        pd.num_samplers        = 1;
        pd.num_readwrite_storage_textures = 1;
        pd.threadcount_x = 8; pd.threadcount_y = 8; pd.threadcount_z = 1;
        if (!bake_pipeline.Create(pd)) {
            fprintf(stderr, "[TerrainWorldHeightmap] normal bake compute pipeline create failed\n");
            normal_tex_.Shutdown();
            tex_.Shutdown();
            return false;
        }

        md::GpuCommandBufferHandle bcmd = SDL_AcquireGPUCommandBuffer(dev);
        GpuComputePass::StorageBindings sb;
        sb.cmd = bcmd;
        sb.rw_textures[0] = { normal_tex_.SDLTexture(), false };
        sb.num_rw_textures = 1;

        GpuComputePass pass;
        pass.Begin(&bake_pipeline, sb);
        if (pass.SDLPass()) {
            SDL_GPUTextureSamplerBinding samp{};
            samp.texture = tex_.SDLTexture();
            samp.sampler = tex_.SDLSampler();
            pass.BindSamplers(0, &samp, 1);

            NormalBakeUBO nubo{};
            nubo.world_params[0] = world_extent_;
            nubo.world_params[1] = (float)N;
            nubo.world_params[2] = height_max_ - height_min_;
            nubo.world_params[3] = height_min_;
            pass.PushUniforms(0, &nubo, sizeof(nubo));

            uint32_t g = (uint32_t)((N + 7) / 8);
            pass.Dispatch(g, g, 1);
            pass.End();
            // Compute pass only wrote mip 0 (readwrite storage binding
            // defaults to level 0) -- fill levels 1..normal_num_levels-1
            // from it now, same call already used for tex_ before #398.
            GpuGenerateMipmaps(bcmd, normal_tex_.SDLTexture());
        } else {
            fprintf(stderr, "[TerrainWorldHeightmap] normal bake SDL_BeginGPUComputePass failed: %s\n", SDL_GetError());
        }
        md::GpuFenceHandle fence = md::GpuDevice::Get().SubmitAndAcquireFence(bcmd);
        if (fence) {
            md::GpuDevice::Get().WaitForFence(fence);
            md::GpuDevice::Get().ReleaseFence(fence);
        }
        bake_pipeline.Destroy();

        fprintf(stderr, "[TerrainWorldHeightmap] normal bake done: %dx%d RG8_SNORM, %u mips\n", N, N, normal_num_levels);
    }

    return true;
}

void TerrainWorldHeightmap::Shutdown(md::GpuDeviceHandle dev) {
    (void)dev;
    normal_tex_.Shutdown();
    tex_.Shutdown();
}
#endif

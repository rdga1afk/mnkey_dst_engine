#pragma once
#ifdef MD_SDL_GPU
#include <SDL3/SDL_gpu.h>
#include <monkey_dust/render/gpu_device.h>
#include <monkey_dust/render/gpu_sampler_texture.h>
// Granite-style terrain migration, Phase 1 (plan at
// /home/rdga1/.claude/plans/serene-pondering-teapot.md): ONE static,
// always-resident GPU texture covering the WHOLE Kenshi world's height
// data, built once from the already-fully-resident CPU TerrainAtlas
// (terrain_gen.cpp/.h) with a full mipmap chain — height LOD comes from
// hardware trilinear mip sampling (a vertex shader's textureLod/
// SampleLevel at a computed fractional mip), never from a per-window
// texture that needs rebuilding. This is the structural fix for bug #3's
// three follow-up rounds this session (character floating/sinking, LOD/
// tier-boundary seams, GPU load from frequent synchronous rebuilds) —
// see the plan's Context section for the full rationale.
//
// Sampled-only usage (never a render target/pipeline color attachment —
// populated via CPU->GPU transfer + SDL_GenerateMipmapsForGPUTexture),
// deliberately a DIFFERENT, lower-risk usage category on Intel Gen9 ANV
// than this codebase's documented render-target-format crash history
// (evsm_shadow.cpp/ssao_system.cpp's R32G32_FLOAT-as-color-target
// precedent, CLAUDE.md Hardware Checklist) — that history is about
// SDL_CreateGPUGraphicsPipeline with a format bound as a color/depth
// attachment, not about a plain sampled texture.
//
// Format: SDL_GPU_TEXTUREFORMAT_R16_UNORM — a lossless, direct match to
// the on-disk source data (world_hmap.r16 is itself raw uint16), no
// R+G-byte-split packing hack the old windowed system needed (this HAL's
// GpuTexture wrapper hardcodes RGBA8; this class bypasses that wrapper
// with hand-rolled SDL_CreateGPUTexture calls, the same pattern already
// shipping in ssao_system.cpp/evsm_shadow.cpp for R32_FLOAT/R16G16_FLOAT).
class TerrainWorldHeightmap {
public:
    // Builds the texture from TerrainAtlas (TerrainAtlas_Loaded() must
    // already be true). Blocking — this is a one-time startup cost (same
    // class of cost as the old system's Init(), already accepted), NOT a
    // per-frame or per-window operation; there is no RebuildRegion
    // equivalent because there is no window to rebuild.
    bool Init(md::GpuDeviceHandle dev);
    void Shutdown(md::GpuDeviceHandle dev);
    bool IsReady() const { return tex_.SDLTexture() != nullptr; }

    md::GpuTextureHandle Texture() const { return tex_.SDLTexture(); }
    SDL_GPUSampler* Sampler() const { return tex_.SDLSampler(); }

    // Full-variant Phase 3 (serene-pondering-teapot.md): world-wide baked
    // normal texture, RG8_SNORM (normal.xz; y = sqrt(1-x^2-z^2) downstream),
    // same resolution as Texture(). Baked once inside Init() via
    // shaders/terrain_worldmap_normal_bake.comp using a FIXED native-texel
    // forward-difference step -- same fix Part A proved at clip-level
    // scale (RenderDoc: normal delta 0.0000 at every boundary), applied
    // here so any two patches sampling this texture at a shared world
    // position agree exactly, regardless of each patch's own tier.
    md::GpuTextureHandle NormalTexture() const { return normal_tex_.SDLTexture(); }
    SDL_GPUSampler* NormalSampler() const { return normal_tex_.SDLSampler(); }

    // Height decode: worldY = HeightMin() + SampledFrac * (HeightMax()-HeightMin())
    float HeightMin() const { return height_min_; }
    float HeightMax() const { return height_max_; }
    // World-metre extent of the square texture (64 zones * CHUNK_SIZE) —
    // divide a world (x,z) by this to get the texture's [0,1] UV.
    float WorldExtent() const { return world_extent_; }
    // Texture resolution (N x N samples, world-grid-deduplicated — see
    // .cpp for the exact zone-boundary-sharing convention, same one
    // TerrainQuadtreeRenderer::s_sample_and_pack already used for a
    // window, generalised here to the whole world).
    int Resolution() const { return res_; }

private:
    GpuTexture tex_;
    GpuTexture normal_tex_;
    float height_min_   = 0.f;
    float height_max_   = 0.f;
    float world_extent_ = 0.f;
    int   res_           = 0;
};
#endif

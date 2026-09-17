#pragma once
#ifdef MD_SDL_GPU
#include <SDL3/SDL_gpu.h>
#include <monkey_dust/render/gpu_pipeline.h>
#include <monkey_dust/render/gpu_compute.h>
#include <monkey_dust/render/gpu_sampler_texture.h>
#include <monkey_dust/world/toroidal_update.h>

// КРОК 7 (docs/KROK7_SKYVIS_AO_DESIGN.md, docs/DAGOR_IMPLEMENTATION_
// PROMPT.md): toroidal heightfield-horizon-sample sky-visibility AO --
// a much lighter reimplementation of Dagor's skyVisibility.cpp concept
// (directional sky-visibility probes, read by the ambient pass),
// sampling the already-resident TerrainWorldHeightmap directly instead
// of tracing a 3D voxel scene (daGI2-class cost, explicitly out of
// scope per this project's own Dagor deep-dive 2 finding). Structured
// like SSAOSystem (Init/UpdatePass/ApplyPass, isolated fullscreen
// multiply-blend onto the swapchain) -- same proven, low-risk pattern,
// not the shared ambient-lighting-pass integration a full daGI2-style
// system would need.
//
// Probe grid: 64x64, 4m spacing, 256m x 256m physical window around the
// camera -- contact/pedestrian AO scale (crevices, overhangs), NOT
// Dagor's km-scale GI clipmap. See the design doc for the full sizing
// rationale. Toroidal update via engine/include/monkey_dust/world/
// toroidal_update.h (КРОК 4) -- only the quad regions that actually left
// the window get re-sampled each update, not the whole grid.
class TerrainSkyVisAO {
public:
    static constexpr int kProbeGridSize = 64;
    static constexpr float kProbeSpacingM = 4.0f;
    static constexpr float kWindowM = kProbeGridSize * kProbeSpacingM; // 256m

    // Allocate the probe texture (R8_UNORM, kProbeGridSize^2) + pipelines.
    bool Init(md::GpuDeviceHandle dev);
    void Shutdown();

    // Call once per frame (or however often the caller wants probes kept
    // fresh -- cheap when the camera hasn't moved far, ToroidalUpdate
    // returns 0 regions and this is a no-op). world_x/world_z: camera
    // position. heightmap_tex/sampler/world_extent/height_min/height_max/
    // resolution: TerrainWorldHeightmap's own accessors, passed through
    // (this class has zero dependency on TerrainWorldHeightmap's actual
    // type, just the raw GPU resource + the same decode convention every
    // other heightmap consumer in this codebase already uses).
    void UpdatePass(md::GpuCommandBufferHandle cmd, float world_x, float world_z,
                     md::GpuTextureHandle heightmap_tex, SDL_GPUSampler* heightmap_sampler,
                     float heightmap_world_extent, float heightmap_height_min,
                     float heightmap_height_max, int heightmap_resolution);

    // Fullscreen multiply-blend onto swapchain_tex, reading gbuf_packed
    // (world position, same packed format terrain_gbuffer_mini.frag
    // writes) to know each pixel's world XZ, sampling the probe grid at
    // that position (toroidally wrapped, world_texel_from-relative).
    void ApplyPass(md::GpuCommandBufferHandle cmd, md::GpuTextureHandle swapchain_tex,
                   int sw, int sh, md::GpuTextureHandle gbuf_packed, SDL_GPUSampler* gbuf_sampler,
                   md::GpuTextureHandle gbuf_depth, SDL_GPUSampler* gbuf_depth_sampler);

    bool IsEnabled() const { return enabled_; }
    void SetEnabled(bool on) { enabled_ = on; }

private:
    bool enabled_ = false;
    md::GpuDeviceHandle dev_ = nullptr;

    // Probe storage -- R8_UNORM, kProbeGridSize x kProbeGridSize, sky-
    // visibility 0..1 (1 = fully open sky, 0 = fully occluded).
    md::GpuTextureHandle probe_tex_ = nullptr;
    SDL_GPUSampler* probe_sampler_ = nullptr;

    // Toroidal cache state -- texel units within probe_tex_ (NOT world
    // metres; world_x/world_z / kProbeSpacingM before feeding ToroidalUpdate).
    md::IPoint2 cur_origin_{0, 0};
    md::IPoint2 main_origin_{0, 0};
    bool origin_initialized_ = false;

    // Last camera world position seen by UpdatePass -- ApplyPass has no
    // camera-position parameter of its own (only gbuf_packed, which gives
    // per-PIXEL world position, not the camera), so this is the fade-band
    // center for the edge-fade in terrain_skyvis_apply.frag.
    float last_cam_x_ = 0.0f, last_cam_z_ = 0.0f;

    GpuComputePipeline update_pipeline_;
    GpuPipeline apply_pipeline_;
};
#endif

#pragma once
// PropRenderer — draws rock props on terrain.
// - Loads a GLB mesh once via Init(glb_path).
// - Draws up to MAX_PROPS instances per frame (no per-frame malloc).
// - If GLB is missing / null, DrawRaw is a no-op.

#include <monkey_dust/render/prop_mesh.h>
#include <monkey_dust/render/gpu_pipeline.h>
#include <monkey_dust/render/gpu_sampler_texture.h>
#include <cstdint>

#ifdef MD_SDL_GPU
#include <SDL3/SDL_gpu.h>
#endif

class PropRenderer {
public:
    static constexpr int   MAX_PROPS = 512;
    static constexpr float SCALE     = 1.5f;  // rock scale in metres

    // Init: load GLB mesh + create pipeline.
    // path=nullptr → no-op, returns false (graceful for missing assets).
    // layer: PropVertex::layer written for every vertex — 0=rock diffuse,
    // 1=vegetation atlas (see PropTexShared).
    bool Init(const char* glb_path, float layer = 0.0f);
    void Shutdown();

    bool IsReady() const { return mesh_.loaded; }

    // Draw N instances inside an already-open render pass.
    // positions_xyz: float array [count * 3], each triple is (x, y, z) world pos.
    // vp16:   column-major mat4 (64 bytes).
    // sun32:  TerrainRenderer::SunParams (32 bytes: vec4 sun_dir_str + vec4 ambient).
    // anim_mode: 0=static, 1=wind sway (vegetation), 2=character bob
    // normals_xyz: parallel array [count*3] of terrain surface normals (G-2 tilt).
    // nullptr = no tilt (props stay upright, e.g. vegetation).
    // quats_xyzw: parallel array [count*4] of real per-instance placement
    // rotations (FeatureScatter — real Kenshi authored orientation, not a
    // terrain-tilt approximation). nullptr = use normals_xyz/tilt instead
    // (existing procedural-prop behaviour, unchanged). Takes priority over
    // normals_xyz when both are non-null (see prop.vert's model_quat.w!=0 check).
    void DrawRaw(
#ifdef MD_SDL_GPU
        SDL_GPURenderPass*    rp,
        md::GpuCommandBufferHandle cmd,
#endif
        const float* positions_xyz,
        int          count,
        const float* vp16,
        const float* sun32,
        float        scale       = SCALE,
        float        anim_mode   = 0.0f,
        float        anim_time   = 0.0f,
        const float* normals_xyz = nullptr,
        const float* quats_xyzw  = nullptr);

private:
    PropMesh    mesh_;
    GpuPipeline pipeline_;
    // Real per-mesh texture (PropMesh::has_custom_tex) — separate GPU
    // resource per PropRenderer instance, unlike the two PropTexShared
    // textures shared across all instances. Bound at set=2 binding=2,
    // always (even a dummy 1x1 white texture when unused) — HD520 sampler
    // binding-order rule: no gaps in fragment sampler slots.
    GpuTexture  tex_custom_;
};

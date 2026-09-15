#pragma once
// VegetationRenderer — mesh LOD + camera-facing billboard LOD for trees (VBfA-OPT-3).
// Near range:  mesh via PropRenderer::DrawRaw().
// Far range:   camera-facing billboard quads (DrawBillboards).
// Billboard texture: placeholder solid colour until veg_tree_billboard.png is available.
// SpeedTree lighting method 0: vertex-precomputed ambient+diffuse, no per-pixel wind.

#include <monkey_dust/render/prop_renderer.h>
#include <monkey_dust/render/gpu_pipeline.h>
#include <monkey_dust/render/ssbo.h>

class VegetationRenderer {
public:
    static constexpr int   MAX_INSTANCES = 512;
    static constexpr float BILLBOARD_W   = 1.8f;  // half-width metres (before scale)
    static constexpr float BILLBOARD_H   = 2.8f;  // height metres

    bool Init(const char* mesh_path);
    void Shutdown();

    bool MeshReady()      const { return mesh_.IsReady(); }
#ifdef MD_SDL_GPU
    bool BillboardReady() const { return bill_pipe_.SDLPipeline() != nullptr; }
#else
    bool BillboardReady() const { return false; }
#endif

    // Draw near instances as 3D mesh (delegates to PropRenderer).
    void DrawMesh(
#ifdef MD_SDL_GPU
        SDL_GPURenderPass* rp, md::GpuCommandBufferHandle cmd,
#endif
        const float* pos3, int count, const float* vp16, const float* sun32,
        float scale, float anim_mode, float anim_time);

    // Draw far instances as cylindrical billboard quads (camera-facing, world-up).
    // pos3:      float[count*3]  — xyz world positions.
    // vp16:      column-major mat4.
    // cam_right: camera right vector (from view matrix row 0).
    // world_up:  (0,1,0) for upright trees.
    // sun_col3:  sun rgb.
    // ambient3:  ambient rgb.
    void DrawBillboards(
#ifdef MD_SDL_GPU
        SDL_GPURenderPass* rp, md::GpuCommandBufferHandle cmd,
#endif
        const float* pos3, int count,
        const float* vp16, const float* cam_right3,
        const float* sun_col3, const float* ambient3);

private:
    PropRenderer mesh_;
#ifdef MD_SDL_GPU
    GpuPipeline  bill_pipe_;
    SSBO         pos_buf_;
#endif
};

#pragma once
#ifdef MD_SDL_GPU
#include <SDL3/SDL_gpu.h>
#include <monkey_dust/render/gpu_pipeline.h>
#include <monkey_dust/render/gpu_hal_types.h>
#include <monkey_dust/render/terrain_tin_mesh.h>

// TIN Etap 2 Stage 1 (docs/TIN_ETAP2_PLAN.md): draws ONE TerrainTinMesh
// into an already-open G-buffer render pass, same output contract as
// TerrainQuadtreeRenderer::DrawNode (shaders/terrain_gbuffer_mini.frag,
// UNMODIFIED -- see terrain_tin.vert's own doc comment for why the 5-
// varying contract lets this work with zero frag-shader changes).
//
// Unlike TerrainQuadtreeRenderer, this is a REAL vertex-buffer draw
// (PropMesh/PropRenderer's pattern, not layout.count=0 procedural) since
// TIN vertices are irregular by construction and must be baked, not
// VTF-sampled at draw time.
class TerrainTinRenderer {
public:
    bool Init(md::GpuDeviceHandle dev);
    void Shutdown(md::GpuDeviceHandle dev);
    bool IsReady() const { return ready_; }

    // zone_origin_x/z: this zone's world-space origin (zx*CHUNK_SIZE_M,
    // zz*CHUNK_SIZE_M) -- added to the mesh's zone-local vertex positions
    // in the vertex shader, same convention TerrainQuadtreeRenderer::
    // DrawNode's node.origin_x/origin_z already uses.
    void DrawMesh(SDL_GPURenderPass* rp, md::GpuCommandBufferHandle cmd,
                  const TerrainTinMesh& mesh, const float* vp16,
                  float zone_origin_x, float zone_origin_z,
                  float cam_x, float cam_y, float cam_z);

private:
    GpuPipeline gbuffer_pipeline_;
    bool ready_ = false;
};
#endif

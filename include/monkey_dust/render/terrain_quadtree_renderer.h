#pragma once
#ifdef MD_SDL_GPU
#include <SDL3/SDL_gpu.h>
#include <monkey_dust/render/gpu_hal_types.h>
#include <monkey_dust/render/gpu_pipeline.h>
#include <monkey_dust/render/gpu_static_buffer.h>
#include <monkey_dust/render/terrain_world_heightmap.h>
#include <monkey_dust/render/terrain_renderer.h>
#include <monkey_dust/world/terrain_quadtree.h>

// 2026-09-05 (docs/TERRAIN_FLAT_LOD_PLAN.md): fixed-depth tiling -- draws
// ONE TerrainQuadtree::VisibleNode per DrawNode call. Vertex-buffer-less
// (layout.count=0) but INDEXED: ONE shared index buffer (filled 16x16 grid,
// terrain_quadtree_mesh.h) built once at Init(), reused for EVERY tile
// everywhere in the world -- only the per-tile UBO (origin, texelSize)
// differs per draw. No skirts/stitched variants: every tile is the same
// depth, so neighbors always share identical vertex density at their
// border. Output contract identical to every prior terrain renderer this
// project has had -- same shaders/terrain_gbuffer_mini.frag, unmodified,
// same packed-normal RGBA32F target.
class TerrainQuadtreeRenderer {
public:
    bool Init(md::GpuDeviceHandle dev);
    void Shutdown(md::GpuDeviceHandle dev);
    bool IsReady() const { return ready_; }

    // Draws ONE tile (filled grid) inside an already-open G-buffer render
    // pass. hmap supplies the world-wide height+normal textures directly
    // (2026-08-24: #398's TerrainHeightClipmap reverted -- see
    // terrain_quadtree.vert's own doc comment for why).
    void DrawNode(SDL_GPURenderPass* rp, md::GpuCommandBufferHandle cmd,
                  const TerrainWorldHeightmap& hmap, const float* vp16,
                  const TerrainQuadtree::VisibleNode& node,
                  float cam_x, float cam_y, float cam_z);

    // Forward (inline) shading revival -- draws node geometry directly into
    // the caller's already-open MAIN color+depth render pass (not an
    // isolated G-buffer), doing full ground-layer shading inline via
    // shaders/terrain_quadtree_forward.frag (shares terrain_quadtree.vert
    // with DrawNode -- only the fragment stage differs). See that shader's
    // doc comment for why this is the revived Variant B architecture
    // (terrain_research/perf/PROGRESS.md, 2026-08-02).
    bool InitForward(md::GpuDeviceHandle dev);
    bool IsForwardReady() const { return forward_ready_; }

    // Binds the forward pipeline + PER-FRAME-CONSTANT fragment resources
    // (ground samplers, zoneGroundLayersTex, sun/fog/world_params/cam
    // uniforms) -- call ONCE before the DrawNodeForward loop each frame,
    // mirroring TerrainRenderer::GetSharedGroundSamplers/
    // ZoneGroundLayersTexture's own "shared across all draws" convention.
    void BeginForward(SDL_GPURenderPass* rp, md::GpuCommandBufferHandle cmd,
                      const TerrainRenderer::SunParams& sun,
                      float cam_x, float cam_y, float cam_z,
                      float world_origin_x, float world_origin_z, float world_to_uv,
                      float fog_far, const float fog_color[3], float fog_near,
                      const TerrainRenderer& ground);

    // Draws ONE tile -- pipeline/fragment resources already bound by
    // BeginForward; this only pushes the per-tile vertex UBO, binds the
    // (world-wide, same for every tile) height/normal vertex samplers, and
    // issues the filled-grid index draw -- same shape as DrawNode.
    void DrawNodeForward(SDL_GPURenderPass* rp, md::GpuCommandBufferHandle cmd,
                         const TerrainWorldHeightmap& hmap, const float* vp16,
                         const TerrainQuadtree::VisibleNode& node,
                         float cam_x, float cam_y, float cam_z);

    // B3 (Granite terrain finding, RENDER_VS_GRANITE_DEEPSEEK_RESEARCH.md):
    // Granite instances up to 512 patches in one draw_indexed; our DrawNode
    // loop issues 1 draw + 1 UBO push PER node instead. This is the
    // instanced replacement for the G-buffer path specifically (DrawNode
    // stays as-is, still used by the forward path and any other caller).
    //
    // Per-node data (origin, texelSize) is read from a
    // SEPARATE vertex shader (terrain_quadtree_batched.vert), not an SSBO:
    // no shader anywhere in this codebase combines vert_samplers>0 with
    // vert_storage_bufs>0, and this exact combination sits in the same
    // territory as multiple already-documented Intel Gen9 ANV silent-fail/
    // hang bugs (GpuPipeline::Create's own vert_storage_bufs+frag_samplers
    // guard, gpu_hal_pipeline.cpp). terrain_quadtree.vert already has 2
    // vertex samplers (heightTex, normalTex); a 3rd sampler reuses an
    // already-proven-safe resource category instead of introducing an
    // untested-on-this-hardware one. Kept as a fully separate vertex
    // shader (not a shared/parameterized one) specifically so the forward
    // pipeline (real frag_samplers=5) never risks inheriting any change
    // made here.
    bool InitBatched(md::GpuDeviceHandle dev);
    bool IsBatchedReady() const { return batched_ready_; }

    // Packs `count` nodes into the CPU-side staging buffer and uploads to
    // nodeDataTex. MUST run inside an already-open SDL_GPUCopyPass on the
    // frame's own command buffer, BEFORE the G-buffer render pass opens --
    // mirrors TerrainVtPageCache::UploadPageMeta's "caller owns the copy
    // pass" convention (SDL_GPU disallows nesting a copy pass inside an
    // already-active render pass). Capped at kMaxBatchedNodes.
    void UploadNodeData(md::GpuDeviceHandle dev, SDL_GPUCopyPass* cp,
                        const TerrainQuadtree::VisibleNode* nodes, int count);

    // Binds the batched pipeline + per-frame-constant vertex resources
    // (height/normal/nodeData samplers, vp/height_range/cam_pos UBO).
    void BeginBatched(SDL_GPURenderPass* rp, md::GpuCommandBufferHandle cmd,
                      const TerrainWorldHeightmap& hmap, const float* vp16,
                      float cam_x, float cam_y, float cam_z);

    // Issues ONE instanced draw_indexed for the filled grid, covering the
    // `count` nodes uploaded via UploadNodeData this frame (count must
    // match the UploadNodeData call this frame).
    void DrawBatched(SDL_GPURenderPass* rp, md::GpuCommandBufferHandle cmd, int count);

    // task БОРГ-VISUAL-3 follow-up (2026-09-06): was 8192 (half of
    // TerrainQuadtree::kMaxNodesPublic's OLD 16384 value) -- fine for
    // gameplay's real node counts, but the editor's 64x64 aerial World3D
    // viewport (now correctly emitting up to the true theoretical zone
    // count after the max_render_distance/kMaxNodesPublic fixes above)
    // measured 71296 visible nodes at a normal 8000m view. Node counts
    // above kMaxBatchedNodes fall back to per-node DrawNode calls (see
    // every DrawBatched call site's own fallback loop) -- 63104 of those
    // individual, non-batched, non-instanced draw calls in a single frame
    // measured live as the direct cause of a 6 FPS regression (confirmed
    // via a temporary qt_count diagnostic, NOT assumed to be texture
    // resolution as first guessed). Matched to TerrainQuadtree::
    // kMaxNodesPublic directly -- the exact theoretical maximum -- so no
    // camera view can ever exceed batched capacity and fall back to the
    // per-node path at all. Cheap: node_data_tex_ scales to ~8.4MB
    // (256 x ~2049 x R32G32B32A32_FLOAT) and the UploadNodeData staging
    // array to ~8.4MB, both static/one-time allocations, not per-frame.
    static constexpr int kMaxBatchedNodes = TerrainQuadtree::kMaxNodesPublic;
    static constexpr int kNodeDataTexWidth = 256;

    // TEMP DEBUG TOOL (2026-09-04, CLAUDE_CONSTITUTION.md sec 7.7): real
    // mesh wireframe overlay -- shares terrain_quadtree.vert with DrawNode/
    // DrawNodeForward (same vertex transform, so lines land exactly on the
    // live geometry), swaps in shaders/terrain_wireframe.frag (solid colour)
    // + GpuRasterState::wireframe=true (SDL_GPU_FILLMODE_LINE). Draws into
    // the caller's already-open MAIN colour+depth pass (same contract as
    // DrawNodeForward), depth_write=false so it never corrupts the real
    // depth buffer. See DrawNodeForward's own doc comment for the pass this
    // must be called from.
    bool InitWireframe(md::GpuDeviceHandle dev);
    bool IsWireframeReady() const { return wireframe_ready_; }
    void DrawNodeWireframe(SDL_GPURenderPass* rp, md::GpuCommandBufferHandle cmd,
                            const TerrainWorldHeightmap& hmap, const float* vp16,
                            const TerrainQuadtree::VisibleNode& node,
                            float cam_x, float cam_y, float cam_z);

private:
    GpuPipeline gbuffer_pipeline_;
    GpuPipeline forward_pipeline_;
    GpuPipeline wireframe_pipeline_;
    GpuPipeline batched_pipeline_;
    GpuStaticBuffer filled_ibo_;
    uint32_t filled_index_count_ = 0;
    bool ready_ = false;
    bool forward_ready_ = false;
    bool batched_ready_ = false;
    bool wireframe_ready_ = false;
    md::GpuTextureHandle node_data_tex_     = nullptr;
    SDL_GPUSampler* node_data_sampler_ = nullptr;
};
#endif

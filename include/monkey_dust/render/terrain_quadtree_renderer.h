#pragma once
#include <chrono>
#include <monkey_dust/platform/md_log.h>
#include <SDL3/SDL_gpu.h>
#include <string>
#include <cstdint>
#include <future>
#include <vector>
#include <monkey_dust/render/gpu_hal_types.h>
#include <monkey_dust/render/gpu_pipeline.h>
#include <monkey_dust/render/gpu_static_buffer.h>
#include <monkey_dust/render/terrain_world_heightmap.h>
#include <monkey_dust/render/terrain_renderer.h>
#include <monkey_dust/world/terrain_quadtree.h>
#include <monkey_dust/world/terrain_lod_tree.h>

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
    // ground: RESOLVE_OPT spatial-split plan, Крок 4 (docs/RESOLVE_OPT.md,
    // 2026-09-19) -- terrain_gbuffer_mini.frag now computes the
    // boundary-mask bit (TS_NeedsCornerBlend), which needs tex_ground/
    // tex_ground_nml/zoneGroundLayersTex bound as fragment samplers.
    void DrawNode(SDL_GPURenderPass* rp, md::GpuCommandBufferHandle cmd,
                  const TerrainWorldHeightmap& hmap, const float* vp16,
                  const TerrainQuadtree::VisibleNode& node,
                  float cam_x, float cam_y, float cam_z,
                  const TerrainRenderer& ground);

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
    // (height/normal/nodeData samplers, vp/height_range/cam_pos UBO) AND
    // (Крок 4, see DrawNode's own doc comment above) the fragment
    // samplers terrain_gbuffer_mini.frag's boundary-mask computation
    // needs -- DrawBatched itself binds nothing per-call, so this is
    // where they must land.
    void BeginBatched(SDL_GPURenderPass* rp, md::GpuCommandBufferHandle cmd,
                      const TerrainWorldHeightmap& hmap, const float* vp16,
                      float cam_x, float cam_y, float cam_z,
                      const TerrainRenderer& ground);

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

    // 2026-09-17 (owner decision): editor's "3D World" tab textures aren't
    // needed for editing work. FIRST attempt promoted the debug wireframe
    // overlay above (full 16x16 internal grid, LINE fillmode) to the tab's
    // real default visual -- live-verified BOTH visually "огидно" (dense
    // mesh reads as solid fill at aerial altitude, line density exceeds
    // screen resolution) AND a real 2 FPS interactive regression (LINE-mode
    // rasterization of ~512 triangle edges/tile times dozens of visible
    // tiles). Replaced with a tile-BOUNDARY-only draw (shaders/
    // terrain_quadtree_boundary.vert, 8 vertices/instance, real LINELIST
    // topology -- not FILLMODE_LINE over a triangle mesh) -- ~64x fewer
    // line segments per tile, reads as a clean grid of tile edges. Same
    // instanced-batched shape as BeginBatched (reuses its node_data_tex_/
    // sampler, InitBatched must run first) so a full 64x64 aerial view
    // draws in ONE instanced call, same reasoning as the shaded path's own
    // БОРГ-VISUAL-3 batching fix. Call BeginBatchedWireframe then
    // DrawBatchedBoundary (NOT DrawBatched -- this pipeline has no index
    // buffer, plain non-indexed instanced draw).
    bool InitBatchedWireframe(md::GpuDeviceHandle dev);
    bool IsBatchedWireframeReady() const { return batched_wireframe_ready_; }
    void BeginBatchedWireframe(SDL_GPURenderPass* rp, md::GpuCommandBufferHandle cmd,
                                const TerrainWorldHeightmap& hmap, const float* vp16,
                                float cam_x, float cam_y, float cam_z);
    void DrawBatchedBoundary(SDL_GPURenderPass* rp, md::GpuCommandBufferHandle cmd, int count);

    // FRAME_AUDIT_2026-09-27 follow-up (audit #2, "no early-Z, no
    // front-to-back order"): depth-only variants of gbuffer_pipeline_/
    // batched_pipeline_ -- IDENTICAL vert_path/frag_path/sampler counts,
    // only depth_only=true (no color target). Kept shader-identical to the
    // color pipelines deliberately: pairing "the same" vertex shader with a
    // DIFFERENT fragment shader let this codebase's driver codegen diverge
    // just enough to fail a later LESS_OR_EQUAL depth test once already
    // (animated_prepass.vert/shadow_csm.frag vs npc_instanced.frag, see
    // npc_render_init.cpp's own doc comment on prepass_pipeline) -- same
    // frag_path here removes that whole risk class. Fills the shared
    // gbuf_depth_ BEFORE the real G-buffer color pass opens with
    // LOAD_OP=LOAD, so the color pass's existing depth_test=true,
    // LESS_OR_EQUAL compare rejects already-occluded fragments via
    // hardware early-Z before terrain_gbuffer_mini.frag runs (confirmed
    // early-Z-eligible: no discard, no gl_FragDepth write).
    bool InitDepthOnly(md::GpuDeviceHandle dev);
    bool IsDepthOnlyReady() const { return depth_only_ready_ && batched_depth_only_ready_; }
    void DrawNodeDepthOnly(SDL_GPURenderPass* rp, md::GpuCommandBufferHandle cmd,
                           const TerrainWorldHeightmap& hmap, const float* vp16,
                           const TerrainQuadtree::VisibleNode& node,
                           float cam_x, float cam_y, float cam_z);
    // Same resource bindings as BeginBatched, depth-only pipeline bound
    // instead -- DrawBatched itself is reused unchanged (it never rebinds
    // a pipeline, only index buffer + draw call).
    void BeginBatchedDepthOnly(SDL_GPURenderPass* rp, md::GpuCommandBufferHandle cmd,
                               const TerrainWorldHeightmap& hmap, const float* vp16,
                               float cam_x, float cam_y, float cam_z);

    // ── #154 phase 3: Kenshi-style LOD (md.set_terrain_lod) ──────────────────────────────────────────────────────────────
    // Quadtree selection by screen-space error (md::TerrainLodTree, tables from fullmap.tif built on a background thread), nodes drawn instanced with
    // geomorph + skirts + 6-neighbour vertex normals computed in the vertex shader (shaders/terrain_quadtree_batched_lod.vert); the finest level (1.8 m)
    // reads a 1536^2 window of fullmap.tif around the camera (recentred when the camera leaves its middle 70 %).
    bool InitLod(md::GpuDeviceHandle dev, const TerrainWorldHeightmap& hmap);
    // Starts the background table build on first use (reads the whole fullmap.tif once); true when the pipeline and the tables are ready.
    bool RequestLod(const TerrainWorldHeightmap& hmap) {
        if (lod_ready_ && !lod_started_) { lod_started_ = true; lod_build_t0_ = std::chrono::steady_clock::now(); lod_tree_.BuildAsync(tif_path_.c_str(), hmap.WorldExtent(), 980.0f, lod_cells_); }
        const bool ready = lod_ready_ && lod_tree_.Ready();
        if (ready && !lod_ready_logged_) { lod_ready_logged_ = true; MD_LOG(MD_LOG_INFO, "[TerrainQuadtreeRenderer] LOD tables ready %lld ms after the build started (flat tiling until then)", (long long)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - lod_build_t0_).count()); }
        return ready;
    }
    // Patch size (quads per node edge): 16 or 64. A change restarts the background table build (LOD is not ready until it finishes -> flat tiling meanwhile).
    void SetLodPatch(int cells) {
        cells = (cells == 64) ? 64 : 16;
        if (cells == lod_cells_) return;
        lod_cells_ = cells;
        if (lod_started_) lod_tree_.BuildAsync(tif_path_.c_str(), 29491.2f, 980.0f, lod_cells_);
    }
    int  LodPatch() const { return lod_cells_; }
    // the LOD's fine height window (R16_UNORM, 1.8 m cells, normalised with HeightMin / HeightRange): the ground cache bake reads heights and normals from it
    bool  LodFineReady() const { return lod_ready_ && fine_tex_ && fine_ox_ >= 0; }
    // true when the fine window holds the heights around (x, z) out to radius r (metres); the window is replaced by a background job after the camera leaves its middle
    // the camera's drift from the window's centre (cells) that starts a recentre; the default is 70 % of the half width. The ground cache rings need the window to hold their reach.
    void  SetFineRecentreCells(int cells) { fine_lim_cells_ = cells; }
    bool  LodFineCovers(float x, float z, float r) const { const float c = 1.8f, n = (float)LodFineCells(); return LodFineReady() && x - r >= fine_ox_ * c && z - r >= fine_oz_ * c && x + r <= (fine_ox_ + n - 1.f) * c && z + r <= (fine_oz_ + n - 1.f) * c; }
    SDL_GPUTexture* LodFineTex() const { return fine_tex_; }
    SDL_GPUSampler* LodFineSampler() const { return fine_sampler_; }
    int   LodFineOx() const { return fine_ox_; }
    int   LodFineOz() const { return fine_oz_; }
    int   LodFineCells() const { return 1536; }
    float LodHeightMin() const { return height_min_; }
    float LodHeightRange() const { return height_range_; }
    // LOD was asked for but cannot work: no Kenshi directory / fullmap.tif (logged as an error). Callers that measure must stop on it.
    bool LodUnavailable() const { return lod_init_failed_ || lod_tree_.Failed(); }
    bool IsLodReady() const { return lod_ready_ && lod_tree_.Ready(); }
    int  SelectLod(const float cam[3], const float frustum_planes[16], const md::TerrainLodParams& p, md::TerrainLodNode* out, int max_out) const {
        return lod_tree_.Select(cam, frustum_planes, p, out, max_out);
    }
    // Inside the caller's open copy pass: recentres the fine window if needed and uploads the node data (2 texels per node, see the .vert).
    void UploadLodNodes(md::GpuDeviceHandle dev, SDL_GPUCopyPass* cp, const md::TerrainLodNode* nodes, int count, float cam_x, float cam_z);
    void BeginBatchedLod(SDL_GPURenderPass* rp, md::GpuCommandBufferHandle cmd, const TerrainWorldHeightmap& hmap, const float* vp16,
                         float cam_x, float cam_y, float cam_z);
    void DrawBatchedLod(SDL_GPURenderPass* rp, md::GpuCommandBufferHandle cmd, int count);
    uint32_t LodTrianglesPerNode() const { return (lod_cells_ == 64 ? lod_index_count64_ : lod_index_count_) / 3; }

private:
    GpuPipeline gbuffer_pipeline_;
    GpuPipeline forward_pipeline_;
    GpuPipeline wireframe_pipeline_;
    GpuPipeline batched_pipeline_;
    GpuPipeline batched_wireframe_pipeline_;
    GpuPipeline gbuffer_depth_only_pipeline_;
    GpuPipeline batched_depth_only_pipeline_;
    GpuStaticBuffer filled_ibo_;
    uint32_t filled_index_count_ = 0;
public:
    uint32_t FilledTrianglesPerNode() const { return filled_index_count_ / 3; } // #142: triangles drawn per visible node (every node shares one IBO)
private:
    std::chrono::steady_clock::time_point lod_build_t0_{};
    bool lod_ready_logged_ = false;
    bool ready_ = false;
    bool forward_ready_ = false;
    bool batched_ready_ = false;
    bool wireframe_ready_ = false;
    bool batched_wireframe_ready_ = false;
    bool depth_only_ready_ = false;
    bool batched_depth_only_ready_ = false;
    md::GpuTextureHandle node_data_tex_     = nullptr;
    SDL_GPUSampler* node_data_sampler_ = nullptr;

    // #154 LOD
    bool lod_ready_ = false;
    bool lod_started_ = false;
    md::TerrainLodTree lod_tree_;
    GpuPipeline lod_pipeline_;
    GpuStaticBuffer lod_ibo_;
    uint32_t lod_index_count_ = 0;
    GpuStaticBuffer lod_ibo64_;
    uint32_t lod_index_count64_ = 0;
    int lod_cells_ = 16;
    bool lod_init_failed_ = false;
    md::GpuTextureHandle fine_tex_ = nullptr;
    SDL_GPUSampler* fine_sampler_ = nullptr;
    int fine_ox_ = -1000000, fine_oz_ = -1000000; // window origin in heightmap cells (1.8 m)
    // the fine window's file read + normalisation runs on a worker (it cost ~38 ms on the render thread at every recentre, ~every 970 m of flight)
    std::future<std::vector<uint16_t>> fine_job_;
    int fine_job_ox_ = 0, fine_job_oz_ = 0;
    bool fine_job_active_ = false;
    int fine_lim_cells_ = 537;
    float height_min_ = 0.f, height_range_ = 1.f;
    std::string tif_path_;
};

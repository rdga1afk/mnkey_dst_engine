#pragma once
#ifdef MD_SDL_GPU
#include <SDL3/SDL_gpu.h>
#include <monkey_dust/render/gpu_pipeline.h>
#include <monkey_dust/render/gpu_static_buffer.h>
#include <monkey_dust/render/terrain_world_heightmap.h>
#include <monkey_dust/render/md_camera.h>

// TPG Stage 1 (docs/TERRAIN_PROJECTED_GRID.md) -- a Johanson-2004-style
// screen-space projected grid, gated by SceneRender::terrain_projected_
// grid_ / md.set_terrain_projected_grid(bool). Sits ALONGSIDE
// TerrainQuadtree/TerrainQuadtreeRenderer, not replacing it yet -- old
// path stays default and unmodified.
//
// NOT to be confused with TerrainShadingProjected (engine/include/
// monkey_dust/render/terrain_shading_projected.h): that class is a
// screen-space G-BUFFER SHADING decoupling technique -- geometry-agnostic
// ("незмінна незалежно від геометрії", engine/src/render/CLAUDE.md),
// unrelated to mesh generation. This class is the opposite: it generates
// MESH geometry that feeds INTO TerrainShadingProjected's existing
// G-buffer pass (same gbuf_color_/gbuf_depth_ contract, unmodified), in
// place of TerrainQuadtreeRenderer::DrawBatched/DrawNode.
//
// Fixed 192x108 screen-space grid (docs/TERRAIN_PROJECTED_GRID.md §0.5):
// no LOD selection, no per-frame node list, no culling -- the grid is
// always fully visible in screen space by construction, so there is
// nothing to select. One static IBO, one draw call per frame.
class TerrainProjectedGrid {
public:
    bool Init(md::GpuDeviceHandle dev);
    void Shutdown(md::GpuDeviceHandle dev);
    bool IsReady() const { return ready_; }

    // Draws the fixed grid inside an already-open G-buffer render pass
    // (same pass TerrainQuadtreeRenderer::DrawNode/DrawBatched draw
    // into -- see TerrainShadingProjected::BeginGBufferPass).
    //
    // CRITICAL -- two DIFFERENT camera representations, do not conflate
    // (docs/TERRAIN_PROJECTED_GRID.md §Критичне виправлення #2):
    //   abs_cam MUST be SceneRender::GraniteAbsCam(cam) -- the
    //     absolute-atlas-space camera already used to build vp for every
    //     other terrain draw call (TerrainWorldHeightmap's native
    //     [0,extent) convention). Feeds the view-proj matrix AND the 4
    //     corner ray-plane intersections.
    //   y_base MUST come from TerrainQuery::GetHeight() called with the
    //     RAW (session-local, un-shifted) camera position, NOT abs_cam's
    //     shifted position -- TerrainQuery::GetHeight applies its OWN
    //     internal absolute-space shift, so feeding it an already-shifted
    //     position double-shifts and silently returns a plausible-looking
    //     but WRONG height (TerrainAtlas_SampleWorld clamps rather than
    //     failing, so this does not crash -- it just produces a subtly
    //     offset base plane).
    void Draw(SDL_GPURenderPass* rp, md::GpuCommandBufferHandle cmd,
              const TerrainWorldHeightmap& hmap,
              const MdCamera& abs_cam, float aspect, float y_base);

    // How many of the 4 screen-corner rays (computed by the most recent
    // Draw() call) had their ray-plane intersection clamped to
    // TerrainQuadtree::kGameplayMaxRenderDistance because the ray was
    // near-parallel to the base plane (near-horizon pitch) -- 0-4. A
    // non-zero count means part of the grid's screen-space extent is
    // degenerate (collapsed onto the distance-capped ring), so the
    // EFFECTIVE resolution is below the nominal 192x108 -- see
    // docs/TERRAIN_PROJECTED_GRID.md §4c. Not a per-vertex count: this
    // design ray-casts only the 4 corners and bilinear-interpolates
    // every other vertex from them, so there is no per-vertex cap state
    // to count.
    int LastCappedCornerCount() const { return capped_corner_count_; }

private:
    GpuPipeline      gbuffer_pipeline_;
    GpuStaticBuffer   grid_ibo_;
    uint32_t          grid_index_count_ = 0;
    bool              ready_ = false;
    int               capped_corner_count_ = 0;
};
#endif

#pragma once

// 2026-09-05 (docs/TERRAIN_FLAT_LOD_PLAN.md): replaced the adaptive CDLOD
// quadtree (recursive per-frame subdivision, continuous geomorph, per-node
// relief-sampled skirts, hash-based neighbor balancing, 16 stitched-IBO
// variants -- ~450 lines across this file + terrain_quadtree.cpp +
// terrain_quadtree_mesh.cpp) with fixed-depth tiling: every visible zone is
// drawn at the SAME depth, so neighbors always share identical vertex
// density at their border and there is no LOD mismatch to hide -- skirts/
// stitching aren't a cheaper alternative here, they're structurally
// unnecessary. Validated on real hardware (Intel HD 520) across 4 diverse
// locations (steep canyon, hills, mountains, gentle terrain): pixel-
// identical output (RMSE<0.5, 0% changed pixels) vs the old adaptive system
// at native leaf resolution, while being 2.3-3.6x cheaper (RenderTotal) --
// the CPU cost of the adaptive system's own per-frame tree-walk + relief
// scan + neighbor balancing exceeded the GPU cost of just drawing more
// triangles at uniform resolution. Full evidence and rejected intermediate
// depths (1, 2 -- both showed real visual regressions on the worst-case
// test point) in that doc.
//
// Quadtree roots are aligned to real Kenshi zones (CHUNK_SIZE=460.8m each,
// re_docs/kenshi/terrain.md's own zone-tile convention). Every emitted tile
// reuses the SAME shared index buffer (terrain_quadtree_mesh.h) -- only
// origin/size differ per draw.
class TerrainQuadtree {
public:
    struct VisibleNode {
        float origin_x, origin_z; // world-space min corner
        float size;               // world-space edge length (16 quads span this)
        int   depth;              // fixed (kFlatLodDepth, terrain_quadtree.cpp)
    };

    using HeightSampleFn = float (*)(float world_x, float world_z);

    // world_extent: TerrainWorldHeightmap::WorldExtent() (must be an exact
    // multiple of CHUNK_SIZE -- true for the real 64-zone Kenshi world).
    void Init(float world_origin_x, float world_origin_z, float world_extent,
              float chunk_size, HeightSampleFn height_sampler);

    // 4 side frustum planes (MdCamera::FrustumPlanes convention, no far
    // plane) + camera position. Returns count written to out[] (capped at
    // max_out). Deterministic function of cam_pos/frustum -- safe to call
    // every frame with no state carried between calls.
    //
    // max_render_distance: full 3D distance (includes altitude, NOT just
    // horizontal) from cam_pos beyond which a zone is culled entirely --
    // see terrain_quadtree.cpp's kFlatMaxRenderDistance doc comment for
    // why the game passes its fog-tied 3000m default. Callers whose camera
    // isn't ground-level gameplay (the editor's 64x64 aerial World3D
    // viewport flies at 8000m+ altitude by design) must pass a value that
    // comfortably covers camera-altitude + world-diagonal, or every zone
    // gets culled and nothing renders -- confirmed live 2026-09-06 (task
    // БОРГ-VISUAL-3 investigation): the editor's 3D World tab rendered
    // pure sky, root-caused to this exact cutoff never having been raised
    // for the aerial-view caller when the flat-LOD system replaced the
    // old adaptive quadtree.
    int SelectVisible(const float cam_pos[3], const float frustum_planes[16],
                       VisibleNode* out, int max_out, float max_render_distance) const;

    // task БОРГ-VISUAL-3 follow-up (2026-09-06): was 16384 (256 zones worth
    // of tiles, at 4^kFlatLodDepth=64 tiles/zone) -- far below the 4096
    // real zones in the world, so any camera view (the editor's 64x64
    // aerial World3D viewport especially) wide enough to have more than
    // 256 zones in frustum silently truncated SelectVisible's output
    // mid-grid-row, which reads as a hard diagonal seam across the visible
    // terrain (world-space row/column cutoff, projected obliquely) and as
    // "can't see the whole map" even at high altitude. Raised to the exact
    // theoretical maximum (64*64 zones * 64 tiles/zone) -- this class of
    // truncation is now structurally impossible regardless of camera
    // altitude/FOV, not just less likely. Cheap: VisibleNode is 16 bytes,
    // so even this worst case is 4MB per static array (game/src has 3,
    // the editor 1 -- see each SelectVisible call site's own comment for
    // why these are static, not stack, arrays).
    static constexpr int kMaxNodesPublic = 64 * 64 * 64;

    // Default max_render_distance for ground-level gameplay callers (game/
    // src's 3 SelectVisible call sites). Must be >= RenderQualityConfig::
    // terrain_cr_m (render_quality.h, default 3000m, 5000m on the highest
    // tier) or terrain visibly pops out of existence before fog fully
    // hides it (fog reaches opacity at fog_far == terrain_cr_m). NOT safe
    // for a high-altitude/aerial camera (see SelectVisible's own doc
    // comment) -- callers like the editor's 64x64 World3D viewport must
    // pass their own, larger value instead of this one.
    static constexpr float kGameplayMaxRenderDistance = 3000.f;

private:
    float world_origin_x_ = 0.f, world_origin_z_ = 0.f;
    float world_extent_   = 0.f;
    float chunk_size_     = 460.8f;
    HeightSampleFn height_sampler_ = nullptr;
};

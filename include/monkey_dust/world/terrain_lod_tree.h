#pragma once
// #154 phase 3: Kenshi-style terrain LOD -- one quadtree over the whole map, each node a 17x17-vertex patch, split where its error on screen is above the
// detail threshold, with a per-node geomorph factor. Facts from the plugin as documented in Project Okran src/render/terrain_lod.rs (MIT): the node error is
// the larger of a tenth of the vertex spacing and the largest morph (an odd vertex's height against the mean of its two neighbours across), the screen error
// is viewport_px * error / distance (the camera's height taken 40 % of the way to the node's centre), the morph factor is (T - ep) / (e - ep) with e / ep the
// node's / its parent's screen error. Levels: level 4 (1843 m) .. level 10 (28.8 m, spacing 1.8 m = the heightmap's own spacing).
//
// The tree itself is stateless per frame (a recursion over the per-node error / height-range tables built once from fullmap.tif); there is no node pool,
// so nothing to merge or drop. Build() reads the whole 16385^2 map once (a few seconds, background thread) and keeps ~16 MB of tables.
#include <atomic>
#include <cstdint>
#include <thread>
#include <vector>

namespace md {

struct TerrainLodParams {
    // Screen-space error threshold in pixels: near_px up to near_dist, rising linearly to far_px at far_dist (MeitouClient docs/formats/settings.md: 5 / 35 px,
    // 7500 -> 15000 Kenshi units = 750 -> 1500 m; Okran's initTerrain call is setDetailThreshold(8, 8, 0), i.e. a constant 8 px).
    float near_px = 5.f, far_px = 35.f;
    float near_dist = 750.f, far_dist = 1500.f;
    float viewport_px = 1080.f;
    float max_render_distance = 4000.f; // nodes whose box is farther than this from the camera are dropped (the camera far plane)
    int   min_level = 4;                // coarsest drawn level
    int   max_level = 10;               // finest level (spacing 1.8 m)
    bool  plugin_ramp = false;          // the far ramp as the plugin computes it (FUN_180003b30, proven in the emulator): the node value is multiplied by t * (near / far) + 1 - t, so the threshold between near_dist and far_dist is near / that factor (hyperbolic), not linear
};

struct TerrainLodNode {
    float origin_x, origin_z; // world min corner
    float size;               // world edge length (16 quads span it)
    float morph;              // 0 = the coarser (parent) geometry, 1 = own geometry
    float skirt_depth;        // metres the border skirt hangs (covers the crack against a neighbour of another level)
    int   level;
    float y_min, y_max;
    int   list_slot = 0;      // ground live pass: 1.. = the slot of the node's biome list (set by the caller before the upload), 0 = none
    // RestrictLodNodes: levels the edge neighbour is coarser (0 = same or finer / not drawn), edges 0 = z min, 1 = z max, 2 = x min, 3 = x max. stitched = no skirt, the
    // border vertices snap to the coarser neighbour's lattice (the plugin's index-buffer border rewrite, plugin_terrain_functions.md).
    unsigned char edge_diff[4] = {0, 0, 0, 0};
    bool  stitched = false;
};

// The screen-error threshold at a distance (the ramp between near_dist and far_dist; plugin_ramp picks the plugin's shape).
float TerrainLodThreshold(const TerrainLodParams& p, float distance);

// Restricted quadtree (the plugin's neighbour rule, FUN_1800042c0): splits every selected leaf whose edge neighbour is more than max_diff levels coarser until none is, and fills edge_diff / stitched.
// The plugin's limit is log2(patch cells) - 2 (Terrain helper +0xf0 = log2(N) - 1, a neighbour below own - (+0xf0) + 1 is split): 2 for 16-cell patches, 4 for 64 (edge_diff holds 3 at most, so callers pass <= 3).
// Children made by a split keep the parent's box heights, morph 0 (their coarse geometry is the parent's grid). Returns the new count (<= max_out; at the cap the
// remaining violations stay and edge_diff may exceed 1). Neighbours that are not in the list (culled, out of range) impose nothing.
int RestrictLodNodes(TerrainLodNode* nodes, int count, int max_out, int max_diff = 1);

class TerrainLodTree {
public:
    static constexpr int kLevels = 11;      // 0..10, tables exist for min_level_built..10
    static constexpr int kMinBuiltLevel = 4;

    ~TerrainLodTree() { Join(); }
    // Starts the table build on a background thread (returns at once). world_extent: 29491.2 m; scale_m: metres at raw 65535 (980).
    // cells: quads per patch edge, 16 or 64 (Kenshi's 'terrain patch size': 16 standard, 64 high quality); 64 = 4x fewer, 16x larger nodes at each spacing.
    void BuildAsync(const char* tif_path, float world_extent, float scale_m, int cells = 16);
    bool Ready() const { return ready_.load(std::memory_order_acquire); }
    bool Failed() const { return failed_.load(std::memory_order_acquire); } // the last table build ended in an error (logged)
    void Join();

    // frustum_planes: 4 side planes as MdCamera::FrustumPlanes (inside = dot >= 0). Returns the node count written (<= max_out).
    int Select(const float cam[3], const float frustum_planes[16], const TerrainLodParams& p, TerrainLodNode* out, int max_out) const;

    static constexpr int kMaxNodes = 65536;

    // The tables for TerrainLodState (the same err / box heights Select reads).
    float Extent() const { return extent_; }
    int   LeafLevel() const { return leaf_; }
    int   Cells() const { return cells_; }
    void  Info(int level, int i, int j, float& err, float& ymin, float& ymax) const { const Level& l = lv_[level]; const size_t k = (size_t)j * l.n + i; err = l.err[k]; ymin = l.ymin[k]; ymax = l.ymax[k]; }

private:
    struct Level {
        int n = 0;                          // nodes per side
        std::vector<float> err, ymin, ymax; // n * n, index j * n + i
    };
    bool Build(const char* tif_path, float world_extent, float scale_m, int cells);
    Level lv_[kLevels];
    float extent_ = 0.f;
    int   cells_ = 16, leaf_ = 10; // patch edge in quads, finest level
    std::thread th_;
    std::atomic<bool> ready_{false};
    std::atomic<bool> failed_{false};
};

} // namespace md

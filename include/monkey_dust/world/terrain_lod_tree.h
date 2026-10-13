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
};

struct TerrainLodNode {
    float origin_x, origin_z; // world min corner
    float size;               // world edge length (16 quads span it)
    float morph;              // 0 = the coarser (parent) geometry, 1 = own geometry
    float skirt_depth;        // metres the border skirt hangs (covers the crack against a neighbour of another level)
    int   level;
    float y_min, y_max;
};

class TerrainLodTree {
public:
    static constexpr int kLevels = 11;      // 0..10, tables exist for min_level_built..10
    static constexpr int kMinBuiltLevel = 4;

    ~TerrainLodTree() { Join(); }
    // Starts the table build on a background thread (returns at once). world_extent: 29491.2 m; scale_m: metres at raw 65535 (980).
    void BuildAsync(const char* tif_path, float world_extent, float scale_m);
    bool Ready() const { return ready_.load(std::memory_order_acquire); }
    void Join();

    // frustum_planes: 4 side planes as MdCamera::FrustumPlanes (inside = dot >= 0). Returns the node count written (<= max_out).
    int Select(const float cam[3], const float frustum_planes[16], const TerrainLodParams& p, TerrainLodNode* out, int max_out) const;

    static constexpr int kMaxNodes = 65536;

private:
    struct Level {
        int n = 0;                          // nodes per side
        std::vector<float> err, ymin, ymax; // n * n, index j * n + i
    };
    bool Build(const char* tif_path, float world_extent, float scale_m);
    Level lv_[kLevels];
    float extent_ = 0.f;
    std::thread th_;
    std::atomic<bool> ready_{false};
};

} // namespace md

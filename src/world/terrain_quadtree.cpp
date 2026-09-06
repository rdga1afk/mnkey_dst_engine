#include <monkey_dust/world/terrain_quadtree.h>
#include <cmath>

namespace {
// Names prefixed Quadtree* to avoid Unity-build anonymous-namespace
// collisions with terrain_patch_grid.cpp's identically-shaped helpers
// (same TU once both land in the same unity batch).
bool QuadtreeAabbInFrustum(float ox, float oz, float size, float ymin, float ymax, const float fp[16]) {
    for (int p = 0; p < 4; ++p) {
        const float* pl = fp + p * 4;
        float px = (pl[0] >= 0.f) ? (ox + size) : ox;
        float pz = (pl[2] >= 0.f) ? (oz + size) : oz;
        float py = (pl[1] >= 0.f) ? ymax : ymin;
        if (pl[0] * px + pl[1] * py + pl[2] * pz + pl[3] < 0.f) return false;
    }
    return true;
}

// docs/TERRAIN_FLAT_LOD_PLAN.md: fixed-depth tiling constants, validated on
// real hardware across 4 diverse locations (steep canyon, hills, mountains,
// gentle terrain) -- see that doc for the full test record and why
// depth=1/2 were rejected (real visual regressions: a black hole through a
// narrow canyon at depth=1, a blocky silhouette + blurred texture patch at
// depth=2) before landing on depth=3 (matches the old adaptive system's own
// native leaf resolution, applied uniformly instead of only near-camera).
constexpr int   kFlatLodDepth          = 3;    // 4^depth patches/zone, texelSize = chunk_size/16/2^depth

void EmitFlatZone(float zone_ox, float zone_oz, float chunk_size, int depth,
                   TerrainQuadtree::VisibleNode* out, int max_out, int& count) {
    int   tiles_per_side = 1 << depth; // 2^depth per axis, 4^depth total
    float tile_size      = chunk_size / (float)tiles_per_side;
    for (int tz = 0; tz < tiles_per_side && count < max_out; ++tz) {
        for (int tx = 0; tx < tiles_per_side && count < max_out; ++tx) {
            out[count].origin_x = zone_ox + (float)tx * tile_size;
            out[count].origin_z = zone_oz + (float)tz * tile_size;
            out[count].size     = tile_size;
            out[count].depth    = depth;
            ++count;
        }
    }
}
} // namespace

void TerrainQuadtree::Init(float world_origin_x, float world_origin_z, float world_extent,
                            float chunk_size, HeightSampleFn height_sampler) {
    world_origin_x_ = world_origin_x;
    world_origin_z_ = world_origin_z;
    world_extent_   = world_extent;
    chunk_size_     = chunk_size;
    height_sampler_ = height_sampler;
}

int TerrainQuadtree::SelectVisible(const float cam_pos[3], const float frustum_planes[16],
                                    VisibleNode* out, int max_out, float max_render_distance) const {
    int count = 0;
    int num_zones = (int)(world_extent_ / chunk_size_ + 0.5f);
    constexpr float kFallbackYMin = -500.f, kFallbackYMax = 4000.f;
    for (int zz = 0; zz < num_zones && count < max_out; ++zz) {
        for (int zx = 0; zx < num_zones && count < max_out; ++zx) {
            float ox = world_origin_x_ + (float)zx * chunk_size_;
            float oz = world_origin_z_ + (float)zz * chunk_size_;
            if (!QuadtreeAabbInFrustum(ox, oz, chunk_size_, kFallbackYMin, kFallbackYMax, frustum_planes))
                continue;
            float cx = ox + chunk_size_ * 0.5f, cz = oz + chunk_size_ * 0.5f;
            float cy = height_sampler_ ? height_sampler_(cx, cz) : cam_pos[1];
            float dx = cam_pos[0] - cx, dy = cam_pos[1] - cy, dz = cam_pos[2] - cz;
            float dist = std::sqrt(dx * dx + dy * dy + dz * dz);
            if (dist > max_render_distance + chunk_size_) continue;
            EmitFlatZone(ox, oz, chunk_size_, kFlatLodDepth, out, max_out, count);
        }
    }
    return count;
}

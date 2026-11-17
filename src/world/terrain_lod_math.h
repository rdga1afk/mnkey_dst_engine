// The node value of the terrain LOD, shared by the stateless selection (terrain_lod_tree.cpp) and the persistent tree (terrain_lod_state.cpp).
#pragma once
#include <monkey_dust/world/terrain_lod_tree.h>
#include <algorithm>
#include <cmath>

namespace md::lodmath {

struct NodeRef { float x0, z0, size, ymin, ymax, err; };

inline bool AabbInFrustum(float x0, float z0, float x1, float z1, float ymin, float ymax, const float fp[16]) {
    for (int p = 0; p < 4; ++p) {
        const float* pl = fp + p * 4;
        const float px = (pl[0] >= 0.f) ? x1 : x0;
        const float pz = (pl[2] >= 0.f) ? z1 : z0;
        const float py = (pl[1] >= 0.f) ? ymax : ymin;
        if (pl[0] * px + pl[1] * py + pl[2] * pz + pl[3] < 0.f) return false;
    }
    return true;
}

// Distance from the camera to the node's box, the camera's height taken 40 % of the way to the node's centre (as the plugin's node error).
inline float NodeDistance(const float cam[3], const NodeRef& n) {
    const float cy = cam[1] + ((n.ymin + n.ymax) * 0.5f - cam[1]) * 0.4f;
    const float dx = std::max(std::max(n.x0 - cam[0], 0.f), cam[0] - (n.x0 + n.size));
    const float dz = std::max(std::max(n.z0 - cam[2], 0.f), cam[2] - (n.z0 + n.size));
    const float dy = std::max(std::max(n.ymin - cy, 0.f), cy - n.ymax);
    return std::max(std::sqrt(dx * dx + dy * dy + dz * dz), 1.f);
}
inline float Threshold(const TerrainLodParams& p, float d) {
    const float t = std::min(std::max((d - p.near_dist) / std::max(p.far_dist - p.near_dist, 1.f), 0.f), 1.f);
    if (p.plugin_ramp) return p.near_px / (t * (p.near_px / p.far_px) + (1.f - t));
    return p.near_px + (p.far_px - p.near_px) * t;
}

}  // namespace md::lodmath

#include <monkey_dust/world/terrain_lod_tree.h>
#include <monkey_dust/io/tiff.h>
#include <algorithm>
#include <cmath>
#include <cstdio>

namespace md {

namespace {
constexpr int   kSamples   = 16385;   // fullmap.tif side
constexpr int   kLeafLevel = 10;      // 1024 nodes per side, 16 cells each
constexpr float kCellM     = 1.8f;    // heightmap spacing

struct Ctx {
    const float*           cam;
    const float*           planes;
    const TerrainLodParams* p;
    const TerrainLodTree*  tree;
    TerrainLodNode*        out;
    int                    max_out;
    int                    count;
};
} // namespace

bool TerrainLodTree::Build(const char* tif_path, float world_extent, float scale_m) {
    extent_ = world_extent;
    md::tiff::Image img;
    std::string err;
    if (!img.Open(tif_path, err)) { fprintf(stderr, "[TerrainLod] %s: %s\n", tif_path, err.c_str()); return false; }
    if (img.Width() != (uint32_t)kSamples || img.Height() != (uint32_t)kSamples || img.Bits() != 16) {
        fprintf(stderr, "[TerrainLod] %s: %ux%u %u-bit, expected %dx%d 16-bit\n", tif_path, img.Width(), img.Height(), img.Bits(), kSamples, kSamples);
        return false;
    }
    std::vector<uint16_t> raw;
    if (!img.Window(0, 0, kSamples, kSamples, 1, raw, err)) { fprintf(stderr, "[TerrainLod] %s\n", err.c_str()); return false; }
    const float k = scale_m / 65535.f;
    auto H = [&](int x, int z) -> float {
        x = std::min(std::max(x, 0), kSamples - 1);
        z = std::min(std::max(z, 0), kSamples - 1);
        return (float)raw[(size_t)z * kSamples + x] * k;
    };
    const unsigned nthreads = std::max(2u, std::min(4u, std::thread::hardware_concurrency()));
    for (int L = kMinBuiltLevel; L <= kLeafLevel; ++L) {
        Level& lv = lv_[L];
        lv.n = 1 << L;
        const size_t cnt = (size_t)lv.n * lv.n;
        lv.err.assign(cnt, 0.f); lv.ymin.assign(cnt, 1e30f); lv.ymax.assign(cnt, -1e30f);
        const int   s      = 1 << (kLeafLevel - L); // samples per node-grid step
        const float floorE = 0.1f * (float)s * kCellM;
        auto work = [&](int j0, int j1) {
            for (int j = j0; j < j1; ++j) {
                for (int i = 0; i < lv.n; ++i) {
                    float emax = floorE, lo = 1e30f, hi = -1e30f;
                    for (int b = 0; b <= 16; ++b) {
                        const int gz = (16 * j + b) * s;
                        for (int a = 0; a <= 16; ++a) {
                            const int gx = (16 * i + a) * s;
                            const float h0 = H(gx, gz);
                            lo = std::min(lo, h0); hi = std::max(hi, h0);
                            if (!((a | b) & 1)) continue;
                            float hm;
                            if ((a & 1) && (b & 1)) hm = 0.5f * (H(gx - s, gz - s) + H(gx + s, gz + s));
                            else if (a & 1)         hm = 0.5f * (H(gx - s, gz) + H(gx + s, gz));
                            else                    hm = 0.5f * (H(gx, gz - s) + H(gx, gz + s));
                            emax = std::max(emax, std::fabs(hm - h0));
                        }
                    }
                    const size_t idx = (size_t)j * lv.n + i;
                    lv.err[idx] = emax; lv.ymin[idx] = lo; lv.ymax[idx] = hi;
                }
            }
        };
        std::vector<std::thread> pool;
        const int per = (lv.n + (int)nthreads - 1) / (int)nthreads;
        for (unsigned t = 0; t < nthreads; ++t) {
            const int j0 = (int)t * per, j1 = std::min(lv.n, j0 + per);
            if (j0 < j1) pool.emplace_back(work, j0, j1);
        }
        for (auto& th : pool) th.join();
    }
    fprintf(stdout, "[TerrainLod] tables built from %s (levels %d..%d)\n", tif_path, kMinBuiltLevel, kLeafLevel);
    return true;
}

void TerrainLodTree::BuildAsync(const char* tif_path, float world_extent, float scale_m) {
    Join();
    ready_.store(false);
    std::string path = tif_path;
    th_ = std::thread([this, path, world_extent, scale_m]() {
        const bool ok = Build(path.c_str(), world_extent, scale_m);
        ready_.store(ok, std::memory_order_release);
    });
}

void TerrainLodTree::Join() {
    if (th_.joinable()) th_.join();
}

static inline bool AabbInFrustum(float x0, float z0, float x1, float z1, float ymin, float ymax, const float fp[16]) {
    for (int p = 0; p < 4; ++p) {
        const float* pl = fp + p * 4;
        const float px = (pl[0] >= 0.f) ? x1 : x0;
        const float pz = (pl[2] >= 0.f) ? z1 : z0;
        const float py = (pl[1] >= 0.f) ? ymax : ymin;
        if (pl[0] * px + pl[1] * py + pl[2] * pz + pl[3] < 0.f) return false;
    }
    return true;
}

namespace {
struct NodeRef { float x0, z0, size, ymin, ymax, err; };

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
    return p.near_px + (p.far_px - p.near_px) * t;
}
} // namespace

int TerrainLodTree::Select(const float cam[3], const float frustum_planes[16], const TerrainLodParams& p, TerrainLodNode* out, int max_out) const {
    if (!Ready()) return 0;
    int count = 0;
    const int min_level = std::max(p.min_level, kMinBuiltLevel), max_level = std::min(p.max_level, kLeafLevel);
    // iterative stack: (level, i, j), the parent's screen error (< 0 = none)
    struct Item { int L, i, j; float ep, perr; };
    std::vector<Item> stack;
    stack.reserve(256);
    const int n0 = 1 << min_level;
    for (int j = 0; j < n0; ++j)
        for (int i = 0; i < n0; ++i) stack.push_back({min_level, i, j, -1.f, 0.f});
    while (!stack.empty() && count < max_out) {
        const Item it = stack.back(); stack.pop_back();
        const Level& lv = lv_[it.L];
        const size_t idx = (size_t)it.j * lv.n + it.i;
        const float size = extent_ / (float)lv.n;
        NodeRef n{ it.i * size, it.j * size, size, lv.ymin[idx], lv.ymax[idx], lv.err[idx] };
        if (!AabbInFrustum(n.x0, n.z0, n.x0 + size, n.z0 + size, n.ymin, n.ymax, frustum_planes)) continue;
        const float d = NodeDistance(cam, n);
        if (d > p.max_render_distance) continue;
        const float e = p.viewport_px * n.err / d;
        const float T = Threshold(p, d);
        if (it.L >= max_level || e <= T) {
            float morph = 1.f;
            if (it.L > min_level && it.ep >= 0.f) {
                const float ep = (it.ep <= e) ? e * 1.001f : it.ep;
                morph = (e == ep) ? 1.f : (T - ep) / (e - ep);
                morph = std::min(std::max(morph, 0.f), 1.f);
            }
            // skirt: the crack against a neighbour of another level is at most about the parent's error (the coarser geometry's deviation)
            const float spacing = size / 16.f;
            const float skirt = std::min(spacing * 4.f, std::max(2.5f * std::max(n.err, it.perr), 0.3f));
            out[count++] = { n.x0, n.z0, size, morph, skirt, it.L, n.ymin, n.ymax };
        } else {
            for (int q = 0; q < 4; ++q) stack.push_back({ it.L + 1, it.i * 2 + (q & 1), it.j * 2 + (q >> 1), e, n.err });
        }
    }
    return count;
}

} // namespace md

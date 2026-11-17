#include <monkey_dust/world/terrain_lod_tree.h>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace md {

namespace {
inline uint64_t Key(int L, int i, int j) { return ((uint64_t)L << 56) | ((uint64_t)(uint32_t)j << 28) | (uint64_t)(uint32_t)i; }

// fixed buffers (no allocation per frame): an open-addressing table over (level, i, j) with a generation stamp (Build needs no clear), the split flags and the output scratch
constexpr int kCap = TerrainLodTree::kMaxNodes, kTab = 2 * kCap;
struct Slot { uint64_t key; int idx; uint32_t gen; };
Slot g_tab[kTab];
uint32_t g_gen = 0;
unsigned char g_split[kCap];
TerrainLodNode g_tmp[kCap];

struct Grid {
    int min_level = 99;
    static inline uint32_t H(uint64_t k) { return (uint32_t)((k * 0x9E3779B97F4A7C15ull) >> 40) & (uint32_t)(kTab - 1); }
    int I(const TerrainLodNode& n) const { return (int)std::lround(n.origin_x / n.size); }
    int J(const TerrainLodNode& n) const { return (int)std::lround(n.origin_z / n.size); }
    void Build(const TerrainLodNode* nodes, int count) {
        if (++g_gen == 0) { std::memset(g_tab, 0, sizeof g_tab); g_gen = 1; }
        min_level = 99;
        for (int k = 0; k < count; ++k) {
            const uint64_t key = Key(nodes[k].level, I(nodes[k]), J(nodes[k]));
            uint32_t h = H(key);
            while (g_tab[h].gen == g_gen && g_tab[h].key != key) h = (h + 1) & (uint32_t)(kTab - 1);
            g_tab[h] = { key, k, g_gen };   // the last node with a key wins
            if (nodes[k].level < min_level) min_level = nodes[k].level;
        }
    }
    int Find(uint64_t key) const {
        uint32_t h = H(key);
        while (g_tab[h].gen == g_gen) { if (g_tab[h].key == key) return g_tab[h].idx; h = (h + 1) & (uint32_t)(kTab - 1); }
        return -1;
    }
    // the leaf covering cell (L, i, j) at its own level or coarser: returns its index and level, -1 when there is none
    int Cover(int L, int i, int j, int& found_level) const {
        for (int l = L; l >= min_level && i >= 0 && j >= 0; --l, i >>= 1, j >>= 1) {
            const int f = Find(Key(l, i, j));
            if (f >= 0) { found_level = l; return f; }
        }
        return -1;
    }
};
constexpr int kDi[4] = {0, 0, -1, 1}, kDj[4] = {-1, 1, 0, 0};
}

int RestrictLodNodes(TerrainLodNode* nodes, int count, int max_out, int max_diff) {
    if (count < 0) count = 0;
    if (count > kCap) count = kCap;
    if (max_out > kCap) max_out = kCap;
    Grid g;
    for (int pass = 0; pass < 32; ++pass) {
        g.Build(nodes, count);
        for (int k = 0; k < count; ++k) g_split[k] = 0;
        int n_split = 0;
        for (int k = 0; k < count; ++k) {
            const TerrainLodNode& n = nodes[k];
            const int i = g.I(n), j = g.J(n);
            for (int e = 0; e < 4; ++e) {
                int fl;
                const int c = g.Cover(n.level, i + kDi[e], j + kDj[e], fl);
                if (c >= 0 && n.level - fl > max_diff && !g_split[c]) { g_split[c] = 1; ++n_split; }
            }
        }
        if (!n_split || count + 3 * n_split > max_out) break;
        int out = 0;
        for (int k = 0; k < count; ++k) if (!g_split[k]) g_tmp[out++] = nodes[k];
        for (int k = 0; k < count; ++k) {
            if (!g_split[k]) continue;
            const TerrainLodNode p = nodes[k];
            for (int q = 0; q < 4; ++q) {
                TerrainLodNode c = p;
                c.size = p.size * 0.5f; c.origin_x = p.origin_x + (float)(q & 1) * c.size; c.origin_z = p.origin_z + (float)(q >> 1) * c.size;
                c.level = p.level + 1; c.morph = 0.f;
                g_tmp[out++] = c;
            }
        }
        std::memcpy(nodes, g_tmp, (size_t)out * sizeof(TerrainLodNode));
        count = out;
    }
    g.Build(nodes, count);
    for (int k = 0; k < count; ++k) {
        TerrainLodNode& n = nodes[k];
        const int i = g.I(n), j = g.J(n);
        for (int e = 0; e < 4; ++e) {
            int fl;
            const int c = g.Cover(n.level, i + kDi[e], j + kDj[e], fl);
            const int d = (c >= 0) ? n.level - fl : 0;
            n.edge_diff[e] = (unsigned char)(d > 3 ? 3 : d);
        }
        n.stitched = true;
        n.skirt_depth = 0.f;
    }
    return count;
}

} // namespace md

#include <monkey_dust/world/terrain_lod_tree.h>
#include <cmath>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace md {

namespace {
inline uint64_t Key(int L, int i, int j) { return ((uint64_t)L << 56) | ((uint64_t)(uint32_t)j << 28) | (uint64_t)(uint32_t)i; }

struct Grid {
    std::unordered_map<uint64_t, int> at;   // (level, i, j) -> node index
    int min_level = 99;
    int I(const TerrainLodNode& n) const { return (int)std::lround(n.origin_x / n.size); }
    int J(const TerrainLodNode& n) const { return (int)std::lround(n.origin_z / n.size); }
    void Build(const TerrainLodNode* nodes, int count) {
        at.clear(); at.reserve((size_t)count * 2); min_level = 99;
        for (int k = 0; k < count; ++k) { at[Key(nodes[k].level, I(nodes[k]), J(nodes[k]))] = k; if (nodes[k].level < min_level) min_level = nodes[k].level; }
    }
    // the leaf covering cell (L, i, j) at its own level or coarser: returns its index and level, -1 when there is none
    int Cover(int L, int i, int j, int& found_level) const {
        for (int l = L; l >= min_level && i >= 0 && j >= 0; --l, i >>= 1, j >>= 1) {
            auto it = at.find(Key(l, i, j));
            if (it != at.end()) { found_level = l; return it->second; }
        }
        return -1;
    }
};
constexpr int kDi[4] = {0, 0, -1, 1}, kDj[4] = {-1, 1, 0, 0};
}

int RestrictLodNodes(TerrainLodNode* nodes, int count, int max_out) {
    Grid g;
    std::vector<char> split;
    for (int pass = 0; pass < 32; ++pass) {
        g.Build(nodes, count);
        split.assign((size_t)count, 0);
        int n_split = 0;
        for (int k = 0; k < count; ++k) {
            const TerrainLodNode& n = nodes[k];
            const int i = g.I(n), j = g.J(n);
            for (int e = 0; e < 4; ++e) {
                int fl;
                const int c = g.Cover(n.level, i + kDi[e], j + kDj[e], fl);
                if (c >= 0 && n.level - fl >= 2 && !split[(size_t)c]) { split[(size_t)c] = 1; ++n_split; }
            }
        }
        if (!n_split || count + 3 * n_split > max_out) break;
        int out = 0;
        std::vector<TerrainLodNode> kids;
        kids.reserve((size_t)n_split * 4);
        for (int k = 0; k < count; ++k) {
            if (!split[(size_t)k]) { nodes[out++] = nodes[k]; continue; }
            const TerrainLodNode p = nodes[k];
            for (int q = 0; q < 4; ++q) {
                TerrainLodNode c = p;
                c.size = p.size * 0.5f; c.origin_x = p.origin_x + (float)(q & 1) * c.size; c.origin_z = p.origin_z + (float)(q >> 1) * c.size;
                c.level = p.level + 1; c.morph = 0.f;
                kids.push_back(c);
            }
        }
        for (const TerrainLodNode& c : kids) nodes[out++] = c;
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

// Persistent terrain LOD quadtree (DE-316). Header: monkey_dust/world/terrain_lod_state.h.
#include <monkey_dust/world/terrain_lod_state.h>
#include "terrain_lod_math.h"
#include <algorithm>

namespace md {

using namespace lodmath;

struct TerrainLodState::Ctx {
    const TerrainLodSource* src;
    const float* cam;
    const float* planes;
    const TerrainLodParams* p;
    TerrainLodNode* out;
    int max_out, count, min_level, max_level, f0;
};

TerrainLodState::TerrainLodState() { Reset(); }

void TerrainLodState::Reset() {
    for (SNode& n : nodes_) n = SNode{};
    for (int b = 0; b < kBlocks; ++b) { free_stack_[b] = kBlocks - 1 - b; blk_parent_[b] = -1; }
    free_n_ = kBlocks; roots_level_ = -1; sweep_pos_ = 0; splits_ = merges_ = 0;
}

int TerrainLodState::Alloc(int parent) {
    if (free_n_ == 0) return -1;
    const int blk = free_stack_[--free_n_];
    blk_parent_[blk] = parent;
    const int first = kMaxRoots + 4 * blk;
    for (int q = 0; q < 4; ++q) { nodes_[first + q].child = -1; nodes_[first + q].last_used = frame_; }
    nodes_[parent].child = first;
    return first;
}

void TerrainLodState::Free(int parent) {
    const int blk = (nodes_[parent].child - kMaxRoots) / 4;
    blk_parent_[blk] = -1;
    free_stack_[free_n_++] = blk;
    nodes_[parent].child = -1;
}

// children that were not visited for 500 frames are released (a slice of the pool per frame; a block whose children still have children waits for those to go first)
void TerrainLodState::Sweep() {
    constexpr int kSlice = kBlocks / 32;
    for (int k = 0; k < kSlice; ++k) {
        const int b = sweep_pos_; sweep_pos_ = (sweep_pos_ + 1) % kBlocks;
        const int parent = blk_parent_[b];
        if (parent < 0 || frame_ - nodes_[parent].last_used <= (uint32_t)kReleaseFrames) continue;
        const int first = kMaxRoots + 4 * b;
        if (nodes_[first].child < 0 && nodes_[first + 1].child < 0 && nodes_[first + 2].child < 0 && nodes_[first + 3].child < 0) Free(parent);
    }
}

int TerrainLodState::EdgeMaxLevel(int node, int level, int side) const {
    const int c = nodes_[node].child;
    if (c < 0) return level;
    static const int kFacing[4][2] = { {0, 1}, {2, 3}, {0, 2}, {1, 3} };
    return std::max(EdgeMaxLevel(c + kFacing[side][0], level + 1, side), EdgeMaxLevel(c + kFacing[side][1], level + 1, side));
}

// FUN_1800045b0: the children are freed only if no edge neighbour is at level >= own + (+0xf0)
bool TerrainLodState::MergeBlocked(const Ctx& c, int level, int i, int j) const {
    static const int kD[4][2] = { {0, -1}, {0, 1}, {-1, 0}, {1, 0} };
    static const int kOpp[4] = { 1, 0, 3, 2 };
    const int n = 1 << level, n0 = 1 << c.min_level;
    for (int s = 0; s < 4; ++s) {
        const int ni = i + kD[s][0], nj = j + kD[s][1];
        if (ni < 0 || nj < 0 || ni >= n || nj >= n) continue;
        int node = (nj >> (level - c.min_level)) * n0 + (ni >> (level - c.min_level));
        int lv = c.min_level;
        while (lv < level && nodes_[node].child >= 0) {
            const int sh = level - lv - 1;
            node = nodes_[node].child + (((ni >> sh) & 1) | (((nj >> sh) & 1) << 1));
            ++lv;
        }
        if (lv < level) continue;   // a coarser leaf
        if (EdgeMaxLevel(node, level, kOpp[s]) >= level + c.f0) return true;
    }
    return false;
}

void TerrainLodState::Visit(Ctx& c, int level, int i, int j, int node, float ep, float perr) {
    if (c.count >= c.max_out) return;
    float err, ymin, ymax;
    c.src->info(c.src->ctx, level, i, j, err, ymin, ymax);
    const float size = c.src->extent / (float)(1 << level);
    const NodeRef n{ i * size, j * size, size, ymin, ymax, err };
    if (!AabbInFrustum(n.x0, n.z0, n.x0 + size, n.z0 + size, ymin, ymax, c.planes)) return;
    const float d = NodeDistance(c.cam, n);
    if (d > c.p->max_render_distance) return;
    nodes_[node].last_used = frame_;
    const float e = c.p->viewport_px * err / d;
    const float T = Threshold(*c.p, d);
    const bool want_split = level < c.max_level && e > T;
    if (nodes_[node].child < 0 && want_split && Alloc(node) >= 0) ++splits_;

    auto emit = [&]() {
        float morph = 1.f;
        if (level > c.min_level && ep >= 0.f) {
            const float epp = (ep <= e) ? e * 1.001f : ep;
            morph = (e == epp) ? 1.f : (T - epp) / (e - epp);
            morph = std::min(std::max(morph, 0.f), 1.f);
        }
        const float spacing = size / (float)c.src->cells;   // the crack against a neighbour of another level is at most about the parent's error
        const float skirt = std::min(spacing * 4.f, std::max(2.5f * std::max(err, perr), 0.3f));
        c.out[c.count++] = { n.x0, n.z0, size, morph, skirt, level, ymin, ymax };
    };
    if (nodes_[node].child < 0) { emit(); return; }

    const int before = c.count, first = nodes_[node].child;
    for (int q = 0; q < 4; ++q) Visit(c, level + 1, i * 2 + (q & 1), j * 2 + (q >> 1), first + q, e, err);
    if (!want_split && nodes_[first].child < 0 && nodes_[first + 1].child < 0 && nodes_[first + 2].child < 0 && nodes_[first + 3].child < 0 && !MergeBlocked(c, level, i, j)) {
        Free(node); ++merges_;
        c.count = before;
        emit();
    }
}

int TerrainLodState::Select(const TerrainLodSource& src, const float cam[3], const float frustum_planes[16], const TerrainLodParams& p, TerrainLodNode* out, int max_out) {
    ++frame_;
    splits_ = merges_ = 0;
    Ctx c{ &src, cam, frustum_planes, &p, out, max_out, 0, std::max(p.min_level, TerrainLodTree::kMinBuiltLevel), std::min(p.max_level, src.leaf_level), 0 };
    c.f0 = TerrainNeighbourLimit(src.cells);   // log2(N) - 1: 3 for 16-cell patches, 5 for 64
    if (c.min_level > 6) c.min_level = 6;
    if (c.min_level != roots_level_) { Reset(); roots_level_ = c.min_level; }
    const int n0 = 1 << c.min_level;
    for (int j = 0; j < n0; ++j)
        for (int i = 0; i < n0; ++i) Visit(c, c.min_level, i, j, j * n0 + i, -1.f, 0.f);
    Sweep();
    return c.count;
}

}  // namespace md

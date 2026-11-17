// DE-316: the persistent terrain LOD quadtree of the plugin (plugin_terrain_functions.md, «CORRECTION 16.11»): nodes live between frames; a leaf splits when its value is above the
// threshold (FUN_180003e00), the four children of a node merge when its value is back below the threshold and no edge neighbour is at level >= own + (+0xf0) (FUN_1800045b0),
// children that were not visited for 500 frames are released. Same node values, threshold ramp, morph and skirt as TerrainLodTree::Select, so a camera that stands still gives
// the same selection (after RestrictLodNodes) as the stateless one; the difference is the hysteresis of the neighbour rule and the cost of a frame that changes little.
#pragma once
#include <monkey_dust/world/terrain_lod_tree.h>
#include <cstdint>

namespace md {

// Where the per-node tables come from (the height map's TerrainLodTree, or a synthetic source in a test).
struct TerrainLodSource {
    float extent = 0.f;
    int   leaf_level = 10, cells = 16;
    const void* ctx = nullptr;
    void (*info)(const void* ctx, int level, int i, int j, float& err, float& ymin, float& ymax) = nullptr;
};

class TerrainLodState {
public:
    static constexpr int kMaxRoots = 4096, kBlocks = 32768, kReleaseFrames = 500;
    TerrainLodState();
    // One frame. Same arguments and output as TerrainLodTree::Select; `frame` counts up by one per call.
    int Select(const TerrainLodSource& src, const float cam[3], const float frustum_planes[16], const TerrainLodParams& p, TerrainLodNode* out, int max_out);
    int LiveBlocks() const { return kBlocks - free_n_; }   // blocks of four nodes in use
    int Splits() const { return splits_; }                 // this frame
    int Merges() const { return merges_; }
    void Reset();
private:
    struct SNode { int32_t child = -1; uint32_t last_used = 0; };   // child = first of four consecutive nodes in the pool, -1 = leaf
    struct Ctx;
    void Visit(Ctx& c, int level, int i, int j, int node, float ep, float perr);
    bool MergeBlocked(const Ctx& c, int level, int i, int j) const;
    int  EdgeMaxLevel(int node, int level, int side) const;   // the finest leaf level along one side of the node (0 z min, 1 z max, 2 x min, 3 x max)
    int  Alloc(int parent);
    void Free(int parent);
    void Sweep();
    SNode nodes_[kMaxRoots + 4 * kBlocks];
    int32_t blk_parent_[kBlocks];
    int32_t free_stack_[kBlocks];
    int free_n_ = 0, roots_level_ = -1, sweep_pos_ = 0, splits_ = 0, merges_ = 0;
    uint32_t frame_ = 0;
};

}  // namespace md

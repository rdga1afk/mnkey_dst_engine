#pragma once
// Ground live pass: the distinct biome lists of the world, one per (cell slots, leaf mask), and the per-sub-cell list index (docs: the leaf list reproduces the original's
// patch rule at every pixel, proven in scripts/kbi1_leaf_vs_patch.py). A sub-cell is one blend map texel: the index image is dim*res x dim*res, row = z, column = x.
#include <monkey_dust/world/kbi1.h>
#include <cstdint>
#include <vector>

namespace md {

struct GroundList {
    int count = 0;                   // 1..4 entries (0 never stored: a leaf without a biome has list 0 = count 0)
    int ids[4] = {0, 0, 0, 0};       // biome "index" values, highest slot first (the first one is the main biome)
    int channels[4] = {0, 0, 0, 0};  // slot = blend map channel
    bool operator==(const GroundList& o) const;
};

class GroundLists {
public:
    // Builds the table and the index image from the reader; list 0 is the empty list.
    void Build(const Kbi1& k);
    int Count() const { return (int)lists_.size(); }
    const GroundList& List(int i) const { return lists_[(size_t)i]; }
    int Side() const { return side_; }
    uint16_t IndexAt(int tx, int tz) const { return index_[(size_t)tz * (size_t)side_ + (size_t)tx]; }
    const std::vector<uint16_t>& IndexImage() const { return index_; }
    // The distinct list indices inside a rectangle of sub-cells (inclusive-exclusive), without the empty list.
    std::vector<int> ListsIn(int tx0, int tz0, int tx1, int tz1) const;
private:
    std::vector<GroundList> lists_;
    std::vector<uint16_t> index_;
    int side_ = 0;
};

}   // namespace md

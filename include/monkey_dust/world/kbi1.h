#pragma once
// blendinfo.dat ("KBI1") of the player's install: which biomes a terrain patch gets. Layout as the original's zone map reader consumes it (our reads of the exe, private notes):
//   "KBI1", int dim (cells per side, 32), int res (sub-cells per cell side, 32), dim*dim cells of 5 int32 biome "index" values (slots 0-4, 0 = none), int L (bytes per subdivided cell,
//   2048), then per cell one mask byte b: (b & 0xe0) == 0x20 -> the cell is one block of that byte, else the block is L bytes (a quadtree in heap order, root at 1, leaves at res*res | morton).
// Mask bits 0-4 = slots 0-4 present. Queries follow the original: the patch's min and max corners must lie in one cell; the mask is the common quadtree ancestor of the two corners.
#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace md {

struct PatchBiomes {
    int count = 0;                 // 0 = none (corners in different cells or outside the world)
    int ids[4] = {0, 0, 0, 0};     // biome "index" values, highest slot first (the first one is the main biome)
    int channels[4] = {0, 0, 0, 0}; // slot of each entry = blend map channel (R,G,B,A = 0..3; 4 = the remainder)
};

class Kbi1 {
public:
    bool Load(const std::string& path);
    bool Loaded() const { return dim_ > 0; }
    int Dim() const { return dim_; }
    int Res() const { return res_; }
    int SubdividedCells() const;
    const std::array<int32_t, 5>& Slots(int cell) const { return ids_[(size_t)cell]; }
    // Positions in the original's centred units (0.1 m, +-147456), the AABB of a patch.
    PatchBiomes Query(float min_x, float min_z, float max_x, float max_z) const;
    // The mask byte for the cell and sub-cell fractions (0..res-1 each) of a point; for tests.
    uint8_t LeafMask(int cell, int sx, int sz) const;
private:
    int dim_ = 0, res_ = 0, block_ = 0;
    std::vector<std::array<int32_t, 5>> ids_;
    std::vector<std::vector<uint8_t>> masks_;   // per cell: 1 byte or block_ bytes
};

}   // namespace md

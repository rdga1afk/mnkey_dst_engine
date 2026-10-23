// The Kenshi terrain colour overview (16384x16384, the 8x8 colour.X.Y.png tiles pasted at native size) as a BC3/DXT5 DDS with a full mip chain
// (16384 .. 1): port of tools/md_stitch_terrain.py + tools/md_bc3_encode.py (range-fit endpoints, opaque alpha). Built once into the derived-data
// cache (~358 MB) and reused while the tiles are unchanged (#201 R3e).
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace md {
// Encodes an RGB image (w,h multiples of 4 NOT required: edge-replicated) as opaque BC3 blocks, row-major block order, 16 bytes per block.
void EncodeBc3Opaque(const uint8_t* rgb, int w, int h, uint8_t* out);
// The whole DDS (header + all mips) of a square power-of-two RGB image given as 8x8 tiles read by `tile(tx,ty,rgb)` (rgb = tile_px*tile_px*3 bytes).
bool BuildTerrainColourDds(const std::string& kenshi_root, std::vector<uint8_t>& dds);
// Path of the cached DDS, built first if needed; "" on failure.
std::string EnsureTerrainColourDds(const std::string& kenshi_root);
}

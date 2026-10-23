// The Kenshi grass/dirt/road paint mask (R grass, G grass2, B dirt, A road) built from the install's 8x8 new_overlay.X.Y.png tiles,
// 4096x4096 RGBA8: each 1024 tile shrunk to 512 by nearest-neighbour (a mask: thin road lines must stay thin). Cached in ~/.cache/monkeydust.
#pragma once
#include <string>
#include <vector>
#include <cstdint>

namespace md {
constexpr int OVERLAY_MASK_SIZE = 4096;
bool BuildOverlayMask(const std::string& kenshi_root, std::vector<uint8_t>& rgba);   // from the tiles, no cache
bool LoadOverlayMask(const std::string& kenshi_root, std::vector<uint8_t>& rgba);    // cache first, then build + store
}

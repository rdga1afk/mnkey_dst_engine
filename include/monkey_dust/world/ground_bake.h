// The flat-ground colour of the whole world baked into one texture (base / slope / grass / dirt / road layers of every zone's biome, blended with the same sequential lerp
// chain and zone-corner bilinear weights as the terrain shader), replacing the live per-pixel blend. Port of tools/md_bake_ground_layers.py main() (#201 R3e). Needs
// TerrainAtlas and BiomeRegistry loaded and the install's ground textures; the result is cached as a BC3 DDS with mips (16384x16384, ~358 MB).
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace md {
// side x side RGB8. The default production size is 16384.
bool BuildGroundBaked(const std::string& kenshi_root, int side, std::vector<uint8_t>& rgb);
// Path of the cached DDS (built first when missing); "" on failure.
std::string EnsureGroundBakedDds(const std::string& kenshi_root);
}

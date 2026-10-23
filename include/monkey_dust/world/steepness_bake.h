// "Smoothed steepness" of the ground, the single source of the slope/cliff weight shared by the ground bake and the live shader (#201 R3e).
// Port of tools/md_bake_ground_layers.py bake_steepness_texture(): per-zone central-difference steepness (1 - 1/|(dh/dx, dh/dz, 1)|) of the
// height field box-blurred with the zone's biome radius (8..16 samples, from the biome's slope_min), mixed by the same bilinear zone-corner
// weights the shader uses. Needs TerrainAtlas and BiomeRegistry loaded (both come from the install). 2048x2048, one byte per texel.
#pragma once
#include <cstdint>
#include <vector>

namespace md {
constexpr int STEEPNESS_SIZE = 2048;
bool BuildSteepnessSmoothed(std::vector<uint8_t>& gray);    // compute
bool LoadSteepnessSmoothed(std::vector<uint8_t>& gray);     // ~/.cache/monkey_dust first, then compute + store
}

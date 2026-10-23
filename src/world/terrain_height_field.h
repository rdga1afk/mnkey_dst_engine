// The world height field as one 8193 x 8193 float array (metres): the TerrainAtlas zones laid side by side, edge samples shared (tools/md_terrain_erode.py load_atlas).
// Shared by the steepness and ground bakes (internal to engine/src/world).
#pragma once
#include <vector>

namespace md {
constexpr int    HF_ZONES = 64, HF_SIZE = 8193;
constexpr double HF_CHUNK = 460.8, HF_EXTENT = HF_ZONES * HF_CHUNK;   // 29491.2 m
int RadiusForSmin(double smin);   // box-blur radius (8..16 height samples) of the biome, from its slope_min (tools/md_bake_ground_layers.py bake_radius_for_smin)
void FillTerrainHeightField(std::vector<float>& h);                  // TerrainAtlas must be loaded
}

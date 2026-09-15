#pragma once
// Shared preamble + cross-TU declarations for the terrain_gen.cpp split
// (biome lookup / atlas I/O / atlas smoothing / noise / chunk build — one
// concern per file). s_atlas_h/s_atlas_loaded/s_atlas_zi/s_atlas_hi and
// s_load_biomemap were originally file-static, shared across several
// functions that now live in different TUs — given external linkage here,
// defined once in terrain_gen_atlas_io.cpp / terrain_gen_biome.cpp,
// mirroring the pattern used for game/src/logic_tick_internal.h.

#include <monkey_dust/world/terrain_gen.h>
#include <monkey_dust/world/chunk_def.h>
#include <monkey_dust/world/biome_system.h>
#include <monkey_dust/world/biome_def.h>
#include <monkey_dust/platform/md_fs.h>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <cstdint>
#include <vector>
#include "stb_image.h"

// ── Terrain Atlas shared layout (defined in terrain_gen_atlas_io.cpp) ────────
constexpr int ATLAS_ZONES  = 64;
constexpr int ATLAS_VERTS  = 129;          // TERRAIN_GRID+1
constexpr int ATLAS_ZBLOCK = ATLAS_VERTS * ATLAS_VERTS; // 16641

extern float s_atlas_h[ATLAS_ZONES * ATLAS_ZONES * ATLAS_ZBLOCK]; // ~260 MB BSS
extern bool  s_atlas_loaded;

int s_atlas_zi(int zx, int zy);
int s_atlas_hi(int zx, int zy, int c, int r);

// ── Biomemap (defined in terrain_gen_biome.cpp) ───────────────────────────────
void s_load_biomemap();

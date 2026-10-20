#pragma once
#include <cmath>

// ── BiomeType ────────────────────────────────────────────────────────────────

enum class BiomeType : unsigned char {
    Desert    = 0,
    Badlands  = 1,
    Grassland = 2,
    Rocky     = 3,
    Swamp     = 4,
    Tundra    = 5,
    Count     = 6
};

// ── BiomeResolver ────────────────────────────────────────────────────────────
// Resolves biome from temperature, moisture, height.
// temperature/moisture: SimplexNoise2 outputs remapped to [0,1].
// height: world-space metres.

struct BiomeResolver {
    static BiomeType Resolve(float temperature, float moisture, float height) {
        if (height > 150.f)              return BiomeType::Rocky;
        if (temperature < 0.22f)         return BiomeType::Tundra;
        if (moisture > 0.62f
            && height < 35.f)            return BiomeType::Swamp;
        if (temperature > 0.58f
            && moisture < 0.32f)         return BiomeType::Desert;
        if (temperature > 0.40f
            && moisture < 0.50f)         return BiomeType::Badlands;
        return BiomeType::Grassland;
    }
};

// ── FactionVoronoi ───────────────────────────────────────────────────────────
// Generates N faction "capital" points from a seed, then returns the nearest
// faction index for any world-space position.
// Designed for MAX_FACTIONS ≤ 64, no dynamic allocation.

static constexpr int VORONOI_MAX_FACTIONS = 64;

struct FactionVoronoi {
    float cx[VORONOI_MAX_FACTIONS];  // capital X (world-space)
    float cz[VORONOI_MAX_FACTIONS];  // capital Z (world-space)
    int   count = 0;

    // Populate N capitals distributed across [0, world_w] × [0, world_h].
    // Uses a seeded LCG so results are deterministic per seed.
    void Init(int n, int seed, float world_w, float world_h) {
        if (n > VORONOI_MAX_FACTIONS) n = VORONOI_MAX_FACTIONS;
        count = n;
        unsigned int s = (unsigned int)seed ^ 0xDEADBEEFu;
        auto lcg = [&]() -> float {
            s = s * 1664525u + 1013904223u;
            return (float)(s >> 8) / (float)(1 << 24);
        };
        // Stratified grid placement: divide world into sqrt(N)×sqrt(N) cells,
        // jitter each capital within its cell for better coverage.
        int cells = (int)ceilf(sqrtf((float)n));
        float cw = world_w / cells;
        float ch = world_h / cells;
        for (int i = 0; i < n; ++i) {
            int row = i / cells, col = i % cells;
            cx[i] = col * cw + lcg() * cw;
            cz[i] = row * ch + lcg() * ch;
        }
    }
};

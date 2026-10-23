#include "terrain_height_field.h"
#include <monkey_dust/world/terrain_gen.h>
#include <algorithm>

#include <cmath>

namespace md {
int RadiusForSmin(double smin) {
    double t = (smin - 0.08) / (0.19 - 0.08); t = std::max(0.0, std::min(1.0, t));
    return (int)std::nearbyint(16 + t * (8 - 16));
}

void FillTerrainHeightField(std::vector<float>& h) {
    h.resize((size_t)HF_SIZE * HF_SIZE);
    for (int i = 0; i < HF_SIZE; ++i) for (int j = 0; j < HF_SIZE; ++j) {
        const int zy = std::min(i / 128, HF_ZONES - 1), zx = std::min(j / 128, HF_ZONES - 1);
        h[(size_t)i * HF_SIZE + j] = TerrainAtlas_GetHeight(zx, zy, j - zx * 128, i - zy * 128);
    }
}
}

#include <monkey_dust/world/steepness_bake.h>
#include <monkey_dust/io/derived_cache.h>
#include <monkey_dust/io/kenshi_dir.h>
#include <monkey_dust/world/biome_def.h>
#include <monkey_dust/world/terrain_gen.h>
#include "terrain_gen_internal.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <set>

namespace md {
namespace {
constexpr int   ZONES = 64, FULL = 8193, OUT = STEEPNESS_SIZE;
constexpr double CHUNK = 460.8, EXTENT = ZONES * CHUNK;

// the python bake keeps the blurred height as float16
float RoundHalf(float f) { return (float)(_Float16)f; }

int RadiusForSmin(double smin) {
    double t = (smin - 0.08) / (0.19 - 0.08); t = std::max(0.0, std::min(1.0, t));
    return (int)std::nearbyint(16 + t * (8 - 16));
}

// (2r+1)^2 box mean, edge-padded
void BoxBlur(const std::vector<float>& a, std::vector<float>& tmp, std::vector<float>& out, int r) {
    const int k = 2 * r + 1;
    for (int y = 0; y < FULL; ++y) {                       // horizontal
        const float* row = &a[(size_t)y * FULL]; float* t = &tmp[(size_t)y * FULL];
        double s = 0; for (int x = -r; x <= r; ++x) s += row[std::clamp(x, 0, FULL - 1)];
        for (int x = 0; x < FULL; ++x) {
            t[x] = (float)(s / k);
            s += row[std::min(x + r + 1, FULL - 1)] - row[std::max(x - r, 0)];
        }
    }
    std::vector<double> s(FULL, 0.0);                       // vertical, all columns at once
    for (int y = -r; y <= r; ++y) { const float* row = &tmp[(size_t)std::clamp(y, 0, FULL - 1) * FULL]; for (int x = 0; x < FULL; ++x) s[x] += row[x]; }
    for (int y = 0; y < FULL; ++y) {
        float* o = &out[(size_t)y * FULL];
        for (int x = 0; x < FULL; ++x) o[x] = RoundHalf((float)(s[x] / k));
        const float* add = &tmp[(size_t)std::min(y + r + 1, FULL - 1) * FULL]; const float* sub = &tmp[(size_t)std::max(y - r, 0) * FULL];
        for (int x = 0; x < FULL; ++x) s[x] += (double)add[x] - sub[x];
    }
}
}

bool BuildSteepnessSmoothed(std::vector<uint8_t>& gray) {
    if (!TerrainAtlas_Loaded() || !BiomeRegistry::Get().Loaded()) return false;
    s_load_biomemap();   // TerrainGen_ResolveBiome reads the install's biomemap.png
    std::vector<float> h((size_t)FULL * FULL);
    for (int i = 0; i < FULL; ++i) for (int j = 0; j < FULL; ++j) {
        const int zy = std::min(i / 128, ZONES - 1), zx = std::min(j / 128, ZONES - 1);
        h[(size_t)i * FULL + j] = TerrainAtlas_GetHeight(zx, zy, j - zx * 128, i - zy * 128);
    }
    int radius[ZONES][ZONES]; std::set<int> radii;
    for (int zy = 0; zy < ZONES; ++zy) for (int zx = 0; zx < ZONES; ++zx) radii.insert(radius[zy][zx] = RadiusForSmin(TerrainGen_ResolveBiome(zx, zy).slope_min));

    std::vector<float> accum((size_t)OUT * OUT, 0.f), wsum((size_t)OUT * OUT, 0.f), tmp(h.size()), blur(h.size());
    const double step_m = EXTENT / (FULL - 1) * 2.0;
    for (int r : radii) {
        BoxBlur(h, tmp, blur, r);
        for (int zy = 0; zy < ZONES; ++zy) for (int zx = 0; zx < ZONES; ++zx) {
            if (radius[zy][zx] != r) continue;
            // output region where this zone acts as one of the 4 corners of the shader's bilinear zone blend
            const int ix0 = std::max(0, (int)std::floor((zx - 0.5) * CHUNK / EXTENT * OUT)), ix1 = std::min(OUT, (int)std::ceil((zx + 1.5) * CHUNK / EXTENT * OUT));
            const int iz0 = std::max(0, (int)std::floor((zy - 0.5) * CHUNK / EXTENT * OUT)), iz1 = std::min(OUT, (int)std::ceil((zy + 1.5) * CHUNK / EXTENT * OUT));
            for (int tz = iz0; tz < iz1; ++tz) for (int tx = ix0; tx < ix1; ++tx) {
                const double wx = (tx + 0.5) / OUT * EXTENT, wz = (tz + 0.5) / OUT * EXTENT;
                const double w = std::clamp(1.0 - std::fabs(wx / CHUNK - 0.5 - zx), 0.0, 1.0) * std::clamp(1.0 - std::fabs(wz / CHUNK - 0.5 - zy), 0.0, 1.0);
                if (!(w > 0.0)) continue;
                const int hx = (int)std::clamp(wx / EXTENT * (FULL - 1), 1.0, (double)FULL - 2), hz = (int)std::clamp(wz / EXTENT * (FULL - 1), 1.0, (double)FULL - 2);
                const double dx = ((double)blur[(size_t)hz * FULL + hx + 1] - blur[(size_t)hz * FULL + hx - 1]) / step_m;
                const double dz = ((double)blur[(size_t)(hz + 1) * FULL + hx] - blur[(size_t)(hz - 1) * FULL + hx]) / step_m;
                const double st = 1.0 - 1.0 / std::sqrt(dx * dx + dz * dz + 1.0);
                accum[(size_t)tz * OUT + tx] += (float)(st * w); wsum[(size_t)tz * OUT + tx] += (float)w;
            }
        }
    }
    gray.resize((size_t)OUT * OUT);
    for (size_t i = 0; i < gray.size(); ++i) gray[i] = (uint8_t)(std::clamp(accum[i] / std::max(wsum[i], 1e-6f), 0.f, 1.f) * 255.f);
    return true;
}

bool LoadSteepnessSmoothed(std::vector<uint8_t>& gray) {
    const std::string dir = KenshiDir();
    if (dir.empty()) return false;
    const uint64_t key = HashFiles({ dir + "/data/newland/land/fullmap.tif", dir + "/data/newland/land/biomemap.png", dir + "/data/gamedata.base" });
    if (CacheRead(key, "steepness_smoothed.u8", gray) && gray.size() == (size_t)OUT * OUT) return true;
    if (!BuildSteepnessSmoothed(gray)) return false;
    CacheWrite(key, "steepness_smoothed.u8", gray.data(), gray.size());
    return true;
}
}

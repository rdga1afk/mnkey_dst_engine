// Grass blade placement as the exe does it (kenshi_x64.exe, Forests::GrassLoader::loadPage 140a3b6b0 and its helpers;
// docs/research/GRASS_EXE_2026-11-15.md). The structure follows PagedGeometry (GrassLoader.cpp, PropertyMaps.cpp,
// RandomTable.h; Copyright (c) 2006 John Judnich, zlib-style licence, notice in engine/THIRD_PARTY.md); this file is an
// altered version, marked as such: the exe differs from upstream in the points listed below, each one reproduced.
//  - RandomTable is the 2006 stride-walk table (60000 floats), never reset per page (upstream resets and uses mt19937).
//  - A slope filter runs before every candidate; a dropped candidate consumes no further draws.
//  - The bilinear density sampler uses (W - 2) cells shifted by half a cell (upstream: W cells).
// DEVIATION (docs/DEVIATIONS.md DE-023): Kenshi seeds the table with GetTickCount(); we fill it from a fixed seed.
// Only the path Kenshi data uses (density map, bilinear) is here: the exe's uniform and nearest paths are never reached.
#pragma once
#include <cstdint>
#include <cstddef>
#include <limits>

namespace md::grass {

// The exe's RandomTable (140a447b0 ctor, 140a44b30 draw). The caller owns the storage (size floats).
struct RandomTable {
    float* table = nullptr;
    uint32_t size = 0;
    uint32_t index = 0;
    uint32_t stride = 1;
    uint32_t counter = 0;

    void Init(float* storage, uint32_t n) { table = storage; size = n; index = 0; stride = 1; counter = 0; }

    // 140a44b30: when fewer than `stride` entries remain, restart at `counter` with a longer stride (1..23).
    float Unit() {
        if (size - stride <= index) {
            counter += 1;
            if (counter == stride) { stride += 1; counter = 0; }
            index = counter;
            if (stride > 0x17) stride = 1;
        }
        const uint32_t r = index;
        index = r + stride;
        return table[r + stride];
    }
    float Range(float a, float b) { return a + (b - a) * Unit(); }
};

// Table value of a 32-bit generator output (140a44850: u32 * 141702180 = 2^-32 * (1 + 2^-32), stored as float).
inline float TableValue(uint32_t u) { return float(double(u) * (1.0 / 4294967295.0)); }

struct Rect { float left, top, right, bottom; };

// 140a6bd10: bilinear sample of a W x H byte map over `b`; 0 outside. Returns 0..1.
inline float DensityBilinear(const uint8_t* px, uint32_t w, uint32_t h, float x, float z, const Rect& b) {
    if (!(b.left <= x && x < b.right && b.top <= z && z < b.bottom)) return 0.0f;
    const float k = 1.0f / 255.0f;   // 14171c9e8 = 0x3b808081
    float xf = float(w - 2) * (x - b.left) / (b.right - b.left);
    xf -= 0.5f;
    xf = float(double(xf) + 1.0);
    float zf = float(h - 2) * (z - b.top) / (b.bottom - b.top);
    zf -= 0.5f;
    zf = float(double(zf) + 1.0);
    const uint32_t xi = uint32_t(int64_t(xf));
    uint32_t zi = uint32_t(int64_t(zf));
    if (xi >= w - 1 || zi >= h - 1) return 0.0f;
    const float xr = xf - float(xi), xri = 1.0f - xr;
    const float zr = zf - float(zi), zri = 1.0f - zr;
    const float v11 = px[zi * w + xi] * k;
    const float v21 = px[zi * w + xi + 1] * k;
    ++zi;
    const float v12 = px[zi * w + xi] * k;
    const float v22 = px[zi * w + xi + 1] * k;
    const float v1 = xri * v11 + xr * v21;
    const float v2 = xri * v12 + xr * v22;
    return zri * v1 + zr * v2;
}

// Terrain callbacks of the page loader (loader +0x10 slope, +0x30 height), user data passed through.
struct TerrainFns {
    float (*slope)(float x, float z, void* user);
    float (*height)(float x, float z, void* user);
    void* user;
};

struct Layer {
    float density;       // blades per square unit, already multiplied by 0.005 and the option (see the research doc)
    float max_slope;     // layer +0x34, default 1000
    float min_y, max_y;  // layer +0x2c / +0x30, 0 = no limit
    const uint8_t* map;  // density map bytes (129 x 129 for Kenshi data)
    uint32_t map_w, map_h;
    Rect map_bounds;     // layer +0x40..+0x4c
};

// loadPage's count: (uint)(density * densityFactor * width * height) with densityFactor = 1.0 (loader +0x4c).
inline uint32_t CandidateCount(const Layer& l, const Rect& page) {
    return uint32_t(l.density * 1.0f * (page.right - page.left) * (page.bottom - page.top));
}

// 140a41690: fills out with (x, z, unit, angle) per accepted blade (4 floats each; out holds 4 * count floats).
// Returns the number of blades. Draw order per candidate: x, z, [slope filter], unit (density test), then on accept the
// height test (draws nothing), unit and angle; on reject two more draws.
inline uint32_t PopulateBilinear(RandomTable& rt, const Layer& l, const TerrainFns& t, const Rect& page, uint32_t count, float* out) {
    const float kTwoPi = 6.28318530717958647692f;
    const bool no_height = l.min_y == 0.0f && l.max_y == 0.0f;
    const float lo = l.min_y == 0.0f ? -std::numeric_limits<float>::infinity() : l.min_y;
    const float hi = l.max_y == 0.0f ? std::numeric_limits<float>::infinity() : l.max_y;
    float* p = out;
    for (uint32_t i = 0; i < count; ++i) {
        const float x = rt.Range(page.left, page.right);
        const float z = rt.Range(page.top, page.bottom);
        if (!(t.slope(x, z, t.user) <= l.max_slope)) continue;
        const float u = rt.Unit();
        const float d = DensityBilinear(l.map, l.map_w, l.map_h, x, z, l.map_bounds);
        if (d <= u) { rt.Unit(); rt.Unit(); continue; }
        if (!no_height) {
            const float y = t.height(x, z, t.user);
            if (y < lo || hi < y) { rt.Unit(); rt.Unit(); continue; }
        }
        p[0] = x;
        p[1] = z;
        p[2] = rt.Unit();
        p[3] = (kTwoPi - 0.0f) * rt.Unit() + 0.0f;
        p += 4;
    }
    return uint32_t((p - out) / 4);
}

} // namespace md::grass

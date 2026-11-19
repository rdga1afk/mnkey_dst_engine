// Grass pages as the exe's pager has them (docs/research/GRASS_EXE_2026-11-15.md: page size = half a zone = 2304 units of 0.1 m, FoliageSystem 1406d3e30): the page a blade belongs to and the visibility test of a page box.
// The original draws only pages that are visible (PagedGeometry pages are scene nodes culled by the engine's frustum test); this is a conservative box-in-frustum test.
#pragma once
#include <cmath>
#include <cstdint>

namespace md::world {

constexpr double kGrassPageUnits = 2304.0;     // 0.1 m units
constexpr double kGrassWorldOrigin = 147456.0; // the grid starts at -147456 (32 zones of 4608)

// Page index of a blade at Kenshi units (x, z): pages are aligned to the zone grid.
inline int64_t GrassPageKey(double x, double z) {
    const int64_t px = (int64_t)std::floor((x + kGrassWorldOrigin) / kGrassPageUnits), pz = (int64_t)std::floor((z + kGrassWorldOrigin) / kGrassPageUnits);
    return px * 4096 + pz;
}

constexpr int kGrassCellsPerSide = 8;                          // a page is 8 x 8 cells of 288 units (28.8 m); the cell is the unit of the grass pass's visibility test

// Cell key: the page key, then the cell inside the page (x * 8 + z). Cells of one page are contiguous in key order.
inline int64_t GrassCellKey(double x, double z) {
    const double ux = x + kGrassWorldOrigin, uz = z + kGrassWorldOrigin, cell = kGrassPageUnits / kGrassCellsPerSide;
    const int64_t px = (int64_t)std::floor(ux / kGrassPageUnits), pz = (int64_t)std::floor(uz / kGrassPageUnits);
    int cx = (int)std::floor((ux - px * kGrassPageUnits) / cell), cz = (int)std::floor((uz - pz * kGrassPageUnits) / cell);
    cx = cx < 0 ? 0 : cx >= kGrassCellsPerSide ? kGrassCellsPerSide - 1 : cx; cz = cz < 0 ? 0 : cz >= kGrassCellsPerSide ? kGrassCellsPerSide - 1 : cz;
    return (px * 4096 + pz) * (kGrassCellsPerSide * kGrassCellsPerSide) + cx * kGrassCellsPerSide + cz;
}

struct PageBox { float lo[3], hi[3]; };   // renderer space, metres

// vp: 16 floats, column-major, clip = vp * (x, y, z, 1). Planes left / right / bottom / top / near / far from the rows of the matrix; the near plane is the weaker -w <= z form so both depth conventions are safe (conservative: a box is only rejected when fully outside one plane).
inline bool PageVisible(const float* vp, const PageBox& b) {
    auto row = [&](int r, int c) { return vp[c * 4 + r]; };
    for (int p = 0; p < 6; ++p) {
        float a[4];
        for (int c = 0; c < 4; ++c) {
            const float w = row(3, c), x = row(0, c), y = row(1, c), z = row(2, c);
            a[c] = p == 0 ? w + x : p == 1 ? w - x : p == 2 ? w + y : p == 3 ? w - y : p == 4 ? w + z : w - z;
        }
        const float px = a[0] >= 0.f ? b.hi[0] : b.lo[0], py = a[1] >= 0.f ? b.hi[1] : b.lo[1], pz = a[2] >= 0.f ? b.hi[2] : b.lo[2];
        if (a[0] * px + a[1] * py + a[2] * pz + a[3] < 0.f) return false;
    }
    return true;
}

}   // namespace md::world

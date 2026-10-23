#include <monkey_dust/world/terrain_colour_dds.h>
#include <monkey_dust/io/derived_cache.h>
#include <stb_image.h>
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <thread>

namespace md {
namespace {
constexpr int GRID = 8, TILE = 2048, SIDE = GRID * TILE, LEVELS = 15;   // 16384 .. 1

uint16_t Pack565(int r, int g, int b) { return (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)); }
void Unpack565(uint16_t v, int& r, int& g, int& b) { r = ((v >> 11) & 31) * 255 / 31; g = ((v >> 5) & 63) * 255 / 63; b = (v & 31) * 255 / 31; }

// one 4x4 block; px = 16 RGB triples in row-major order
void EncodeBlock(const uint8_t* px, uint8_t* o) {
    int mn[3] = { 255, 255, 255 }, mx[3] = { 0, 0, 0 };
    for (int i = 0; i < 16; ++i) for (int k = 0; k < 3; ++k) { mn[k] = std::min<int>(mn[k], px[i * 3 + k]); mx[k] = std::max<int>(mx[k], px[i * 3 + k]); }
    const uint16_t a = Pack565(mx[0], mx[1], mx[2]), b = Pack565(mn[0], mn[1], mn[2]);
    uint16_t c0 = a <= b ? b : a, c1 = a <= b ? a : b;
    if (a == b) c0 = (uint16_t)std::min<int>(c1 + 1, 0xFFFF);
    int col[4][3]; Unpack565(c0, col[0][0], col[0][1], col[0][2]); Unpack565(c1, col[1][0], col[1][1], col[1][2]);
    for (int k = 0; k < 3; ++k) { col[2][k] = (2 * col[0][k] + col[1][k]) / 3; col[3][k] = (col[0][k] + 2 * col[1][k]) / 3; }
    uint32_t idx = 0;
    for (int t = 0; t < 16; ++t) {
        int best = 0, bd = 1 << 30;
        for (int c = 0; c < 4; ++c) {
            const int dr = px[t * 3] - col[c][0], dg = px[t * 3 + 1] - col[c][1], db = px[t * 3 + 2] - col[c][2];
            const int d = dr * dr + dg * dg + db * db;
            if (d < bd) { bd = d; best = c; }
        }
        idx |= (uint32_t)best << (t * 2);
    }
    o[0] = o[1] = 255; memset(o + 2, 0, 6);   // alpha0 = alpha1 = 255: opaque
    o[8] = c0 & 0xFF; o[9] = c0 >> 8; o[10] = c1 & 0xFF; o[11] = c1 >> 8;
    memcpy(o + 12, &idx, 4);
}

// 2x2 box, truncating like numpy's mean(...).astype(uint8)
void Down(const std::vector<uint8_t>& s, int dim, std::vector<uint8_t>& d) {
    const int n = dim / 2; d.resize((size_t)n * n * 3);
    for (int y = 0; y < n; ++y) for (int x = 0; x < n; ++x) for (int k = 0; k < 3; ++k) {
        const size_t i = ((size_t)(2 * y) * dim + 2 * x) * 3 + k;
        d[((size_t)y * n + x) * 3 + k] = (uint8_t)((s[i] + s[i + 3] + s[i + (size_t)dim * 3] + s[i + (size_t)dim * 3 + 3] ) / 4);
    }
}
size_t LevelBytes(int dim) { const size_t b = (size_t)((dim + 3) / 4); return b * b * 16; }
}

void EncodeBc3Opaque(const uint8_t* rgb, int w, int h, uint8_t* out) {
    const int bw = (w + 3) / 4, bh = (h + 3) / 4;
    uint8_t px[48];
    for (int by = 0; by < bh; ++by) for (int bx = 0; bx < bw; ++bx) {
        for (int y = 0; y < 4; ++y) for (int x = 0; x < 4; ++x)
            memcpy(px + (y * 4 + x) * 3, rgb + ((size_t)std::min(by * 4 + y, h - 1) * w + std::min(bx * 4 + x, w - 1)) * 3, 3);
        EncodeBlock(px, out + ((size_t)by * bw + bx) * 16);
    }
}

bool EncodeBc3Dds(const uint8_t* rgb, int side, std::vector<uint8_t>& dds) {
    int levels = 1; for (int d = side; d > 1; d /= 2) ++levels;
    std::vector<size_t> off(levels + 1); off[0] = 128;
    for (int l = 0; l < levels; ++l) off[l + 1] = off[l] + LevelBytes(side >> l);
    dds.assign(off[levels], 0);
    uint32_t hd[32] = { 0x20534444u, 124, 0x1 | 0x2 | 0x4 | 0x80000 | 0x20000, (uint32_t)side, (uint32_t)side, (uint32_t)LevelBytes(side), 0, (uint32_t)levels };
    hd[19] = 32; hd[20] = 0x4; hd[21] = 0x35545844u; hd[27] = 0x1000 | 0x400000 | 0x8;
    memcpy(dds.data(), hd, 128);
    std::vector<uint8_t> cur(rgb, rgb + (size_t)side * side * 3), nxt;
    for (int l = 0, dim = side; l < levels; ++l, dim /= 2) {
        // block rows of this level in parallel
        const int bw = (dim + 3) / 4, bh = bw, nt = (int)std::max(1u, std::min(4u, std::thread::hardware_concurrency()));
        std::vector<std::thread> th;
        for (int t = 0; t < nt; ++t) th.emplace_back([&, t] {
            for (int by = t; by < bh; by += nt) {
                const int y0 = by * 4, rows = std::min(4, dim - y0);
                if (rows == 4) EncodeBc3Opaque(&cur[(size_t)y0 * dim * 3], dim, 4, &dds[off[l] + (size_t)by * bw * 16]);
                else { std::vector<uint8_t> pad((size_t)dim * 4 * 3); for (int r = 0; r < 4; ++r) memcpy(&pad[(size_t)r * dim * 3], &cur[(size_t)std::min(y0 + r, dim - 1) * dim * 3], (size_t)dim * 3); EncodeBc3Opaque(pad.data(), dim, 4, &dds[off[l] + (size_t)by * bw * 16]); }
            }
        });
        for (auto& x : th) x.join();
        if (dim > 1) { Down(cur, dim, nxt); cur.swap(nxt); }
    }
    return true;
}

bool BuildTerrainColourDds(const std::string& root, std::vector<uint8_t>& dds) {
    size_t level_off[LEVELS + 1]; level_off[0] = 128;
    for (int l = 0; l < LEVELS; ++l) level_off[l + 1] = level_off[l] + LevelBytes(SIDE >> l);
    dds.assign(level_off[LEVELS], 0);
    // header exactly as tools/md_bc3_encode.py
    uint32_t hd[32] = { 0x20534444u, 124, 0x1 | 0x2 | 0x4 | 0x80000 | 0x20000, SIDE, SIDE, (uint32_t)LevelBytes(SIDE), 0, LEVELS };
    hd[19] = 32; hd[20] = 0x4; hd[21] = 0x35545844u; hd[27] = 0x1000 | 0x400000 | 0x8;
    memcpy(dds.data(), hd, 128);

    std::vector<uint8_t> l10((size_t)16 * 16 * 3), l11((size_t)8 * 8 * 3);   // levels 10 (16x16) and 11 (8x8) are smaller than a block per tile: assembled from all tiles
    std::atomic<int> next{0}, failed{0};
    auto work = [&]() {
        for (;;) {
            const int t = next++; if (t >= GRID * GRID) return;
            const int tx = t % GRID, ty = t / GRID;
            char p[512]; snprintf(p, sizeof(p), "%s/data/newland/land/overlaymaps/colour.%d.%d.png", root.c_str(), tx, ty);
            int w = 0, h = 0, c = 0; uint8_t* rgba = stbi_load(p, &w, &h, &c, 4);
            if (!rgba || w != TILE || h != TILE) { if (rgba) stbi_image_free(rgba); ++failed; fprintf(stderr, "[TerrainColour] bad tile %s\n", p); return; }
            std::vector<uint8_t> cur((size_t)TILE * TILE * 3), nxt;
            for (size_t i = 0; i < (size_t)TILE * TILE; ++i) memcpy(&cur[i * 3], rgba + i * 4, 3);
            stbi_image_free(rgba);
            for (int l = 0, dim = TILE; l <= 11; ++l, dim /= 2) {
                // this tile's blocks of level l: block rows [ty*dim/4, ..) of the level's (SIDE>>l)/4-wide block grid
                const int lw = SIDE >> l, bw_total = (lw + 3) / 4;
                if (dim >= 4) {
                    std::vector<uint8_t> blk((size_t)(dim / 4) * (dim / 4) * 16);
                    EncodeBc3Opaque(cur.data(), dim, dim, blk.data());
                    for (int by = 0; by < dim / 4; ++by)
                        memcpy(&dds[level_off[l] + ((size_t)(ty * (dim / 4) + by) * bw_total + tx * (dim / 4)) * 16], &blk[(size_t)by * (dim / 4) * 16], (size_t)(dim / 4) * 16);
                }
                if (l == 10) for (int y = 0; y < 2; ++y) memcpy(&l10[((size_t)(ty * 2 + y) * 16 + tx * 2) * 3], &cur[(size_t)y * 2 * 3], 6);
                if (l == 11) { memcpy(&l11[((size_t)ty * GRID + tx) * 3], cur.data(), 3); break; }
                Down(cur, dim, nxt); cur.swap(nxt);
            }
        }
    };
    std::vector<std::thread> th; const int n = (int)std::max(1u, std::min(4u, std::thread::hardware_concurrency()));
    for (int i = 0; i < n; ++i) th.emplace_back(work);
    for (auto& t : th) t.join();
    if (failed) return false;
    // levels 10 .. 14: whole images (16x16, 8x8, then 4x4, 2x2, 1x1 by the same box filter)
    std::vector<uint8_t> cur = l10, nxt; int dim = 16;
    for (int l = 10; l < LEVELS; ++l, dim /= 2) {
        if (l == 11) cur = l11;
        else if (l > 11) { Down(cur, dim * 2, nxt); cur.swap(nxt); }
        EncodeBc3Opaque(cur.data(), dim, dim, &dds[level_off[l]]);
    }
    return true;
}

std::string EnsureTerrainColourDds(const std::string& root) {
    std::vector<std::string> files;
    for (int ty = 0; ty < GRID; ++ty) for (int tx = 0; tx < GRID; ++tx) { char p[512]; snprintf(p, sizeof(p), "%s/data/newland/land/overlaymaps/colour.%d.%d.png", root.c_str(), tx, ty); files.push_back(p); }
    const uint64_t key = HashFiles(files) ^ 0x5eed0002ull;   // salt = encoder revision (2: the palette colour is no longer computed in uint8)
    const std::string path = CachePath(key, "terrain_colour.dds");
    if (path.empty()) return {};
    if (FILE* f = fopen(path.c_str(), "rb")) { fseek(f, 0, SEEK_END); const long n = ftell(f); fclose(f); if ((size_t)n == 128 + [] { size_t t = 0; for (int l = 0; l < LEVELS; ++l) t += LevelBytes(SIDE >> l); return t; }()) return path; }
    std::vector<uint8_t> dds;
    if (!BuildTerrainColourDds(root, dds) || !CacheWrite(key, "terrain_colour.dds", dds.data(), dds.size())) return {};
    return path;
}
}

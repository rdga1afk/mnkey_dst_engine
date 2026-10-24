#include <monkey_dust/world/ground_bake.h>
#include <monkey_dust/world/overlay_mask_build.h>
#include <monkey_dust/world/terrain_colour_dds.h>
#include <monkey_dust/io/dds_decode.h>
#include <monkey_dust/io/derived_cache.h>
#include <monkey_dust/world/biome_def.h>
#include <monkey_dust/world/terrain_gen.h>
#include "terrain_gen_internal.h"
#include "terrain_height_field.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <chrono>
#include <thread>

// The python bake stores the heights, the steepness chain and its accumulators as float16; _Float16 round trips below reproduce that so the port can be checked against it.
namespace md {
namespace {
inline float H(float x) { return (float)(_Float16)x; }
inline double Smooth(double e0, double e1, double x) { double t = std::clamp((x - e0) / (e1 - e0), 0.0, 1.0); return t * t * (3.0 - 2.0 * t); }

constexpr double DDS_UV_SCALE = 1.0 / 5000.0;
constexpr double TS_CLIFF_MIN = 0.25, TS_CLIFF_BLEND = 0.15;
constexpr int PAD = 17;   // largest box radius (16) + 1 neighbour

// A ground texture level, float RGB 0..1, with wrapping bilinear sampling.
struct Level { int w = 0, h = 0; std::vector<float> px; };

// numpy sample_bilinear_wrap (u, v >= 0 here). Level sizes are powers of two (DXT textures and their halvings): wrap by mask.
inline void SampleWrap(const Level& t, double u, double v, double out[3]) {
    const double fu = (u - (double)(long)u) * t.w - 0.5, fv = (v - (double)(long)v) * t.h - 0.5;
    const int x0 = (int)fu - (fu < 0), y0 = (int)fv - (fv < 0);
    const double tx = fu - x0, ty = fv - y0;
    const int wm = t.w - 1, hm = t.h - 1, xa = x0 & wm, xb = (x0 + 1) & wm, ya = y0 & hm, yb = (y0 + 1) & hm;
    const float *c00 = &t.px[((size_t)ya * t.w + xa) * 3], *c10 = &t.px[((size_t)ya * t.w + xb) * 3], *c01 = &t.px[((size_t)yb * t.w + xa) * 3], *c11 = &t.px[((size_t)yb * t.w + xb) * 3];
    for (int k = 0; k < 3; ++k) { const double top = c00[k] * (1 - tx) + c10[k] * tx, bot = c01[k] * (1 - tx) + c11[k] * tx; out[k] = top * (1 - ty) + bot * ty; }
}

Level HalfDown(const Level& s) {   // 2x2 box, float16 storage (the python chain)
    Level d; d.w = s.w / 2; d.h = s.h / 2; d.px.resize((size_t)d.w * d.h * 3);
    for (int y = 0; y < d.h; ++y) for (int x = 0; x < d.w; ++x) for (int k = 0; k < 3; ++k) {
        const size_t i = ((size_t)(2 * y) * s.w + 2 * x) * 3 + k;
        d.px[((size_t)y * d.w + x) * 3 + k] = H((s.px[i] + s.px[i + 3] + s.px[i + (size_t)s.w * 3] + s.px[i + (size_t)s.w * 3 + 3]) / 4.f);
    }
    return d;
}

struct Key { int idx, bucket; bool operator<(const Key& o) const { return idx != o.idx ? idx < o.idx : bucket < o.bucket; } };
}

bool BuildGroundBaked(const std::string& root, int side, std::vector<uint8_t>& rgb) {
    if (!TerrainAtlas_Loaded() || !BiomeRegistry::Get().Loaded()) return false;
    s_load_biomemap();
    BiomeRegistry& reg = BiomeRegistry::Get();

    std::vector<uint8_t> mask;   // 4096^2 RGBA
    const auto t_start = std::chrono::steady_clock::now();
    auto secs = [&] { return std::chrono::duration<double>(std::chrono::steady_clock::now() - t_start).count(); };
    if (!LoadOverlayMask(root, mask)) return false;
    const int MW = OVERLAY_MASK_SIZE;

    std::vector<float> height; FillTerrainHeightField(height);
    // summed-area table of the edge-padded height field: the box blur of any window in O(1)
    const int PS = HF_SIZE + 2 * PAD;
    std::vector<double> sat((size_t)(PS + 1) * (PS + 1), 0.0);
    for (int y = 0; y < PS; ++y) {
        double row = 0; const float* src = &height[(size_t)std::clamp(y - PAD, 0, HF_SIZE - 1) * HF_SIZE];
        for (int x = 0; x < PS; ++x) { row += src[std::clamp(x - PAD, 0, HF_SIZE - 1)]; sat[(size_t)(y + 1) * (PS + 1) + x + 1] = sat[(size_t)y * (PS + 1) + x + 1] + row; }
    }
    auto boxmean = [&](int hx, int hz, int r) {   // mean of the (2r+1)^2 window centred on height sample (hx,hz), float16 like the python field
        const int x0 = hx + PAD - r, x1 = hx + PAD + r + 1, z0 = hz + PAD - r, z1 = hz + PAD + r + 1;
        const double s = sat[(size_t)z1 * (PS + 1) + x1] - sat[(size_t)z0 * (PS + 1) + x1] - sat[(size_t)z1 * (PS + 1) + x0] + sat[(size_t)z0 * (PS + 1) + x0];
        return H((float)(s / ((2.0 * r + 1) * (2.0 * r + 1))));
    };

    // per-zone biome, in the python iteration order (zy outer, zx inner) for the first-seen mip pick
    const BiomeDef* zb[HF_ZONES][HF_ZONES]; int zr[HF_ZONES][HF_ZONES];
    for (int zy = 0; zy < HF_ZONES; ++zy) for (int zx = 0; zx < HF_ZONES; ++zx) { zb[zy][zx] = &TerrainGen_ResolveBiome(zx, zy); zr[zy][zx] = RadiusForSmin(zb[zy][zx]->slope_min); }

    const double meters_per_texel = HF_EXTENT / side;
    std::map<Key, int> key_level;            // (texture, log2 bucket) -> mip level chosen by the first request
    std::map<int, int> need_level_of_first;  // unused placeholder to keep the map type simple
    struct Want { int idx; double tiling; };
    for (int zy = 0; zy < HF_ZONES; ++zy) for (int zx = 0; zx < HF_ZONES; ++zx) {
        const BiomeDef& b = *zb[zy][zx];
        const Want w[5] = { { b.tex_base, b.tile_base_x }, { b.tex_slope, b.tile_slope_x }, { b.tex_grass, b.tile_grass_x }, { b.tex_dirt, b.tile_dirt_x }, { b.tex_road, b.tile_road_x } };
        for (const Want& q : w) {
            const double target = std::max(4.0, (5000.0 / std::max(q.tiling, 1e-6)) / meters_per_texel);
            const Key k{ q.idx, (int)std::nearbyint(std::log2(std::max(target, 1.0))) };
            if (!key_level.count(k)) key_level[k] = (int)std::lround(target * 1000);   // remember the first target (x1000) until the level is resolved below
        }
    }
    // decode each texture once, build its chain, keep the one level picked per key
    std::map<Key, Level> picked;
    {
        std::map<int, std::vector<Key>> by_tex; for (auto& kv : key_level) by_tex[kv.first.idx].push_back(kv.first);
        for (auto& kv : by_tex) {
            int w = 0, h = 0; std::vector<uint8_t> px;
            const char* path = reg.GroundTexPath(kv.first);
            if (!path || !DecodeDdsRgba(path, w, h, px)) { fprintf(stderr, "[GroundBake] cannot decode ground texture %d: %s\n", kv.first, path ? path : "(null)"); return false; }
            if ((w & (w - 1)) || (h & (h - 1))) { fprintf(stderr, "[GroundBake] ground texture %d is %dx%d, not a power of two\n", kv.first, w, h); return false; }
            Level cur; cur.w = w; cur.h = h; cur.px.resize((size_t)w * h * 3);
            for (size_t i = 0; i < (size_t)w * h; ++i) for (int k = 0; k < 3; ++k) cur.px[i * 3 + k] = H(px[i * 4 + k] / 255.f);
            std::vector<Level> chain; chain.push_back(std::move(cur));
            while (std::min(chain.back().w, chain.back().h) > 4) chain.push_back(HalfDown(chain.back()));
            for (const Key& k : kv.second) {
                const double target = key_level[k] / 1000.0;
                const Level* best = &chain.back();
                for (const Level& m : chain) { best = &m; if (m.h <= target) break; }
                picked[k] = *best;
            }
        }
    }

    struct ZoneLayers { const Level* lv[5]; double tu[5], tv[5]; };   // base, slope, grass, dirt, road
    std::vector<ZoneLayers> zl((size_t)HF_ZONES * HF_ZONES);
    for (int zy = 0; zy < HF_ZONES; ++zy) for (int zx = 0; zx < HF_ZONES; ++zx) {
        const BiomeDef& b = *zb[zy][zx]; ZoneLayers& z = zl[(size_t)zy * HF_ZONES + zx];
        const int ti[5] = { b.tex_base, b.tex_slope, b.tex_grass, b.tex_dirt, b.tex_road };
        const double tx_[5] = { b.tile_base_x, b.tile_slope_x, b.tile_grass_x, b.tile_dirt_x, b.tile_road_x }, ty_[5] = { b.tile_base_y, b.tile_slope_y, b.tile_grass_y, b.tile_dirt_y, b.tile_road_y };
        for (int k = 0; k < 5; ++k) {
            const double target = std::max(4.0, (5000.0 / std::max(tx_[k], 1e-6)) / meters_per_texel);
            z.lv[k] = &picked.at(Key{ ti[k], (int)std::nearbyint(std::log2(std::max(target, 1.0))) });
            z.tu[k] = DDS_UV_SCALE * tx_[k]; z.tv[k] = DDS_UV_SCALE * ty_[k];
        }
    }
    fprintf(stderr, "[GroundBake] inputs ready after %.1f s (%zu texture levels)\n", secs(), picked.size());
    rgb.assign((size_t)side * side * 3, 0);
    const double step_m = H((float)(HF_EXTENT / (HF_SIZE - 1) * 2.0));   // the python division by a float16 scalar
    std::atomic<int> next_row{0};
    auto work = [&]() {
        for (;;) {
            const int pair = next_row++; if (pair * 2 >= side) return;
            struct Memo { int hx = -1, hz = -1, zone = -1; float slope_w = 0; } memo[64];   // the smoothed slope weight depends on (height sample, zone) only: shared by the 2x2 texels of one sample
            for (int tx = 0; tx < side; ++tx) for (int tz = pair * 2; tz < std::min(side, pair * 2 + 2); ++tz) {
                const double wz = (tz + 0.5) / side * HF_EXTENT;
                const double wx = (tx + 0.5) / side * HF_EXTENT;
                const double zcx = wx / HF_CHUNK - 0.5, zcz = wz / HF_CHUNK - 0.5;
                const int hx = (int)std::clamp(wx / HF_EXTENT * (HF_SIZE - 1), 1.0, (double)HF_SIZE - 2), hz = (int)std::clamp(wz / HF_EXTENT * (HF_SIZE - 1), 1.0, (double)HF_SIZE - 2);
                // raw steepness (float32 like the python), same for every zone
                const float h_xp = height[(size_t)hz * HF_SIZE + hx + 1], h_xn = height[(size_t)hz * HF_SIZE + hx - 1], h_zp = height[(size_t)(hz + 1) * HF_SIZE + hx], h_zn = height[(size_t)(hz - 1) * HF_SIZE + hx];
                const float dxr = (h_xp - h_xn) / 7.2f, dzr = (h_zp - h_zn) / 7.2f;   // float32 steps like the python (the smoothed field below divides in float16)
                const float steep_raw = 1.f - 1.f / std::sqrt(dxr * dxr + dzr * dzr + 1.f);
                const double cliff_raw = Smooth(TS_CLIFF_MIN - TS_CLIFF_BLEND, TS_CLIFF_MIN, (double)steep_raw);
                // overlay mask, bilinear clamp
                double m[4];
                {
                    const double fu = std::clamp(wx / HF_EXTENT, 0.0, 1.0) * MW - 0.5, fv = std::clamp(wz / HF_EXTENT, 0.0, 1.0) * MW - 0.5;
                    const long x0 = (long)std::floor(fu), y0 = (long)std::floor(fv); const double tfx = fu - x0, tfy = fv - y0;
                    const long xa = std::clamp<long>(x0, 0, MW - 1), xb = std::clamp<long>(x0 + 1, 0, MW - 1), ya = std::clamp<long>(y0, 0, MW - 1), yb = std::clamp<long>(y0 + 1, 0, MW - 1);
                    for (int k = 0; k < 4; ++k) {
                        const double c00 = mask[((size_t)ya * MW + xa) * 4 + k] / 255.f, c10 = mask[((size_t)ya * MW + xb) * 4 + k] / 255.f, c01 = mask[((size_t)yb * MW + xa) * 4 + k] / 255.f, c11 = mask[((size_t)yb * MW + xb) * 4 + k] / 255.f;
                        m[k] = (c00 * (1 - tfx) + c10 * tfx) * (1 - tfy) + (c01 * (1 - tfx) + c11 * tfx) * tfy;
                    }
                }
                const double gw = std::max(m[0], m[1]), dw = m[2], rw = m[3];
                float acc[3] = { 0, 0, 0 }, wsum = 0;
                const int zx0 = (int)std::floor(zcx), zz0 = (int)std::floor(zcz);
                for (int zy = zz0; zy <= zz0 + 1; ++zy) for (int zx = zx0; zx <= zx0 + 1; ++zx) {   // zy outer, zx inner: the python's zone order
                    if (zx < 0 || zx >= HF_ZONES || zy < 0 || zy >= HF_ZONES) continue;
                    const double weight = std::clamp(1.0 - std::fabs(zcx - zx), 0.0, 1.0) * std::clamp(1.0 - std::fabs(zcz - zy), 0.0, 1.0);
                    if (!(weight > 0.0)) continue;
                    const BiomeDef& b = *zb[zy][zx]; const int r = zr[zy][zx];
                    Memo& me = memo[(hx * 31 + hz * 17 + zy * 7 + zx) & 63];
                    if (me.hx != hx || me.hz != hz || me.zone != zy * HF_ZONES + zx) {
                        // smoothed steepness in float16 arithmetic like the python
                        const _Float16 sxp = (_Float16)boxmean(hx + 1, hz, r), sxn = (_Float16)boxmean(hx - 1, hz, r), szp = (_Float16)boxmean(hx, hz + 1, r), szn = (_Float16)boxmean(hx, hz - 1, r);
                        const float dx = H(H((float)sxp - (float)sxn) / (float)(_Float16)step_m), dz = H(H((float)szp - (float)szn) / (float)(_Float16)step_m);
                        const float nl = H(std::sqrt(H(H(H(dx * dx) + H(dz * dz)) + 1.f)));
                        const float steep = H(1.f - H(1.f / nl));
                        auto sm16 = [](double e0, double e1, float x) { const float t = H(std::clamp(H(H(x - H((float)e0)) / H((float)(e1 - e0))), 0.f, 1.f)); return H(H(t * t) * H(3.f - H(2.f * t))); };
                        me.slope_w = H(sm16(b.slope_min - b.slope_fade, b.slope_min, steep) * sm16(b.slope_max + b.slope_fade, b.slope_max, steep));
                        me.hx = hx; me.hz = hz; me.zone = zy * HF_ZONES + zx;
                    }
                    const float slope_w = me.slope_w;
                    const float sw = (float)(slope_w * (1.0f - (float)cliff_raw));
                    const ZoneLayers& z = zl[(size_t)zy * HF_ZONES + zx];
                    double cb[3], cs[3], cg[3], cd[3], cr_[3];
                    SampleWrap(*z.lv[0], wx * z.tu[0], wz * z.tv[0], cb);
                    SampleWrap(*z.lv[1], wx * z.tu[1], wz * z.tv[1], cs);
                    SampleWrap(*z.lv[2], wx * z.tu[2], wz * z.tv[2], cg);
                    SampleWrap(*z.lv[3], wx * z.tu[3], wz * z.tv[3], cd);
                    SampleWrap(*z.lv[4], wx * z.tu[4], wz * z.tv[4], cr_);
                    for (int k = 0; k < 3; ++k) {
                        double col = cb[k] * (1.0 - gw) + cg[k] * gw;
                        col = col * (double)(1.0f - sw) + cs[k] * sw;
                        col = col * (1.0 - dw) + cd[k] * dw;
                        col = col * (1.0 - rw) + cr_[k] * rw;
                        acc[k] = H((float)((double)acc[k] + col * weight));
                    }
                    wsum = H((float)((double)wsum + weight));
                }
                uint8_t* o = &rgb[((size_t)tz * side + tx) * 3];
                const float ws = std::max(wsum, 1e-6f);
                for (int k = 0; k < 3; ++k) o[k] = (uint8_t)(H(std::clamp(H(acc[k] / ws), 0.f, 1.f) * 255.f));
            }
        }
    };
    std::vector<std::thread> th; const int n = (int)std::max(1u, std::min(4u, std::thread::hardware_concurrency()));
    for (int i = 0; i < n; ++i) th.emplace_back(work);
    for (auto& t : th) t.join();
    fprintf(stderr, "[GroundBake] %dx%d baked in %.1f s\n", side, side, secs());
    return true;
}

std::string EnsureGroundBakedDds(const std::string& root) {
    // inputs: the ground textures (through the biome registry paths), the overlay tiles, heights and the biome map; one hash over their files
    std::vector<std::string> files = { root + "/data/gamedata.base", root + "/data/newland/land/fullmap.tif", root + "/data/newland/land/biomemap.png" };
    BiomeRegistry& reg = BiomeRegistry::Get();
    for (int i = 0; i < reg.GroundTexCount(); ++i) if (const char* p = reg.GroundTexPath(i)) files.push_back(p);
    for (int ty = 0; ty < 8; ++ty) for (int tx = 0; tx < 8; ++tx) { char p[512]; snprintf(p, sizeof(p), "%s/data/newland/land/overlaymaps/new_overlay.%d.%d.png", root.c_str(), tx, ty); files.push_back(p); }
    const uint64_t key = HashFiles(files) ^ 0x6a0d0002ull;   // salt = bake revision (2: BC1 instead of BC3, constant opaque alpha)
    const std::string path = CachePath(key, "ground_baked.dds");
    if (path.empty()) return {};
    if (FILE* f = fopen(path.c_str(), "rb")) { fclose(f); fprintf(stderr, "[Cache] hit  %016llx/ground_baked.dds\n", (unsigned long long)key); return path; }
    std::vector<uint8_t> rgb, dds;
    if (!BuildGroundBaked(root, 16384, rgb)) return {};
    if (!EncodeBc1Dds(rgb.data(), 16384, dds)) return {};
    rgb = {}; 
    if (!CacheWrite(key, "ground_baked.dds", dds.data(), dds.size())) return {};
    return path;
}
}

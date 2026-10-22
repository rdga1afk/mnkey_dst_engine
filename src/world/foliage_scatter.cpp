// See foliage_scatter.h: port of Project Okran (MIT, Copyright (c) 2026 brayniac6-glitch) src/world/foliage.rs, behaviour for behaviour.
#include <monkey_dust/world/foliage_scatter.h>
#include <algorithm>
#include <cmath>
#include <functional>
#include <monkey_dust/io/tiff.h>
#include <stb_image.h>
#include <map>
#include <random>
#include <memory>

namespace md::world {

Mt::Mt(uint32_t seed) {
    state_[0] = seed;
    for (int i = 1; i < 624; ++i) state_[i] = (state_[i - 1] ^ (state_[i - 1] >> 30)) * 0x6c078965u + (uint32_t)i;
}

void Mt::Reload() {
    for (int i = 0; i < 624; ++i) {
        const uint32_t y = (state_[i] & 0x80000000u) | (state_[(i + 1) % 624] & 0x7fffffffu);
        uint32_t v = state_[(i + 397) % 624] ^ (y >> 1);
        if (y & 1) v ^= 0x9908b0dfu;
        state_[i] = v;
    }
    index_ = 0;
}

uint32_t Mt::NextU32() {
    if (index_ >= 624) Reload();
    uint32_t y = state_[index_++];
    y ^= y >> 11;
    y ^= (y << 7) & 0x9d2c5680u;
    y ^= (y << 15) & 0xefc60000u;
    return y ^ (y >> 18);
}

float Mt::Range(float a, float b) {
    const double u = (double)NextU32();
    return (float)(u * (1.0 / 4294967296.0) * (double)(b - a) + (double)a);
}

uint32_t Mt::Int(uint32_t n) {
    uint32_t m = (n >> 1) | n;
    m |= m >> 2; m |= m >> 4; m |= m >> 8; m |= m >> 16;
    for (;;) {
        const uint32_t v = NextU32() & m;
        if (v <= n) return v;
    }
}

int32_t ParseIntOgre(const std::string& s) {
    size_t i = 0;
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
    size_t j = i;
    if (j < s.size() && (s[j] == '-' || s[j] == '+')) ++j;
    const size_t d0 = j;
    while (j < s.size() && s[j] >= '0' && s[j] <= '9') ++j;
    if (j == d0) return 0;
    return (int32_t)std::strtol(s.substr(i, j - i).c_str(), nullptr, 10);
}

namespace {
constexpr int32_t kFoliageLayer = 59, kFoliageMesh = 60, kBiomes = 28;

float FlOrInt(const md::fcs::GameData& r, const char* k) {
    auto f = r.floats.find(k); if (f != r.floats.end()) return f->second;
    auto i = r.ints.find(k); if (i != r.ints.end()) return (float)i->second;
    return 0.f;
}
bool Bl(const md::fcs::GameData& r, const char* k) { auto b = r.bools.find(k); return b != r.bools.end() && b->second; }
}

Foliage LoadFoliage(const md::fcs::GameDataContainer& data) {
    Foliage f;
    std::unordered_map<std::string, int> mesh_index, layer_index;
    std::function<int(const md::fcs::GameData&, int)> mesh_of = [&](const md::fcs::GameData& r, int depth) -> int {
        auto it = mesh_index.find(r.string_id);
        if (it != mesh_index.end()) return it->second;
        const int i = (int)f.meshes.size();
        mesh_index[r.string_id] = i;
        FoliageMesh m;
        m.record = r.string_id;
        auto file_of = [&](const char* k) { auto fl = r.files.find(k); return fl == r.files.end() ? std::string() : fl->second; };
        m.mesh = file_of("mesh"); m.texture = file_of("texture map"); m.leaves_mesh = file_of("leaves mesh"); m.leaves_texture = file_of("leaves texture");
        m.max_slope = FlOrInt(r, "max slope"); m.min_slope = FlOrInt(r, "min slope");
        m.min_altitude = FlOrInt(r, "min altitude"); m.max_altitude = FlOrInt(r, "max altitude");
        m.floating = Bl(r, "floating"); m.clustered = Bl(r, "clustered");
        m.cluster_num_min = std::trunc(FlOrInt(r, "cluster num min")); m.cluster_num_max = std::trunc(FlOrInt(r, "cluster num max"));
        m.cluster_radius_min = FlOrInt(r, "cluster radius min"); m.cluster_radius_max = FlOrInt(r, "cluster radius max");
        m.limit_to_grass = Bl(r, "limit to grass areas");
        m.child_cluster_radius = FlOrInt(r, "child cluster radius"); m.road_avoidance = FlOrInt(r, "road avoidance"); m.grass_spot = FlOrInt(r, "grass spot");
        m.avoid_towns = Bl(r, "avoid towns");
        { const auto* bt = r.Refs("building type"); m.building_type = bt && !bt->empty(); }
        m.material_type = (int)FlOrInt(r, "material type");
        m.min_height = FlOrInt(r, "min height"); m.max_height = FlOrInt(r, "max height");
        m.vertical_offset_min = FlOrInt(r, "vertical offset min"); m.vertical_offset_max = FlOrInt(r, "vertical offset max");
        f.meshes.push_back(std::move(m));
        if (depth < 4) {
            std::vector<int> kids;
            if (const auto* refs = r.Refs("meshes"))
                for (const auto& ref : *refs) {
                    const md::fcs::GameData* c = data.Get(ref.first);
                    if (c && c->kind == kFoliageMesh) kids.push_back(mesh_of(*c, depth + 1));
                }
            f.meshes[i].children = std::move(kids);
        }
        return i;
    };
    std::unordered_map<std::string, int> grass_index;
    for (const md::fcs::GameData* biome : data.OfType(kBiomes)) {
        std::vector<int> list, grass_list, blade_list;
        { auto ix = biome->ints.find("index"); if (ix != biome->ints.end()) f.biome_by_index[ix->second] = biome->string_id; }
        if (const auto* flist = biome->Refs("foliage"))
            for (const auto& ref : *flist) {
                const md::fcs::GameData* l = data.Get(ref.first);
                if (!l) continue;
                if (const auto* grefs = l->Refs("grass"))
                    for (const auto& gr : *grefs) {
                        const md::fcs::GameData* g = data.Get(gr.first);
                        if (!g) continue;
                        auto gi = grass_index.find(gr.first);
                        if (gi == grass_index.end()) {
                            Grass x;
                            x.record = gr.first;
                            auto fl = [&](const char* k, float dflt) { auto it = g->floats.find(k); if (it != g->floats.end()) return it->second; auto ii = g->ints.find(k); return ii != g->ints.end() ? (float)ii->second : dflt; };
                            x.density = fl("density", 0) * 0.005f;
                            x.min_altitude = fl("min altitude", 0); x.max_altitude = fl("max altitude", 0); x.max_slope = fl("max slope", 0);
                            x.noise_scale = fl("noise scale", 1); x.zero_cutoff = fl("zero cutoff", 0);
                            x.blackout_noise_scale = fl("blackout noise scale", 1); x.blackout_zero_cutoff = fl("blackout zero cutoff", 0);
                            x.cap = fl("cap", 1); x.brightness_boost = fl("brightness boost", 1);
                            x.blackout = Bl(*g, "blackout");
                            x.min_height = fl("min height", 1); x.max_height = fl("max height", 1);
                            { auto sp = g->files.find("grass sprite"); if (sp != g->files.end()) x.sprite = sp->second; }
                            gi = grass_index.emplace(gr.first, (int)f.grass.size()).first;
                            f.grass.push_back(x);
                        }
                        grass_list.push_back(gi->second);
                        if (gr.second[0] == 0) blade_list.push_back(gi->second);
                    }
                const auto* mrefs = l->Refs("meshes");
                if (!mrefs || mrefs->empty()) continue;
                auto li = layer_index.find(ref.first);
                int idx;
                if (li != layer_index.end()) idx = li->second;
                else {
                    FoliageLayer fl;
                    fl.record = l->string_id;
                    fl.seed_base = ParseIntOgre(l->string_id);
                    for (const auto& mr : *mrefs) {
                        const md::fcs::GameData* m = data.Get(mr.first);
                        if (!m) continue;
                        fl.meshes.emplace_back(mesh_of(*m, 0), (float)mr.second[0]);
                    }
                    { auto v = l->ints.find("visibility range"); fl.visibility_range = v == l->ints.end() ? 0 : v->second; }
                    fl.wind = Bl(*l, "wind");
                    idx = (int)f.layers.size();
                    f.layers.push_back(std::move(fl));
                    layer_index[ref.first] = idx;
                }
                list.push_back(idx);
            }
        f.biome_layers[biome->string_id] = std::move(list);
        f.biome_grass[biome->string_id] = std::move(grass_list);
        f.biome_blades[biome->string_id] = std::move(blade_list);
    }
    return f;
}

bool LoadTowns(const std::string& kenshi_dir, const md::fcs::GameDataContainer& data, std::vector<Town>& out, std::string& err) {
    std::unordered_map<std::string, size_t> at;
    for (const char* folder : { "Newwworld", "rebirth" }) {
        md::fcs::File file; md::fcs::Error e;
        const std::string path = kenshi_dir + "/data/newland/leveldata/" + folder + "/leveldata.level";
        if (!md::fcs::ReadFile(path, file, e)) { err = e.Message(); return false; }
        for (const auto& rec : file.records) {
            if (rec.kind != 38) continue;
            for (const auto& in : rec.instances) {
                const md::fcs::GameData* t = data.Get(in.target);
                if (!t || t->kind != 13) continue;
                Town tw;
                tw.x = in.position[0]; tw.z = in.position[2];
                auto ty = t->ints.find("type"); tw.kind = ty == t->ints.end() ? 0 : ty->second;
                tw.no_foliage = tw.kind == 8 ? 0.f : FlOrInt(*t, "no-foliage range");
                auto it = at.find(in.target);
                if (it == at.end()) { at[in.target] = out.size(); out.push_back(tw); } else out[it->second] = tw;
            }
        }
    }
    return true;
}

namespace {

int NearestTown(const std::vector<Town>& towns, float px, float pz) {   // Okran town.rs nearest(towns, p, 10, None): the nearest that is not of type 8
    int best = -1; float d = 3.4028235e38f;
    for (size_t i = 0; i < towns.size(); ++i) {
        if (towns[i].kind == 8) continue;
        const float dx = towns[i].x - px, dz = towns[i].z - pz;
        const float e = dx * dx + dz * dz;
        if (e < d) { best = (int)i; d = e; }
    }
    return best;
}

void Normalise2(float x, float y, float* ox, float* oy) {
    const float l = std::sqrt(x * x + y * y);
    if (l > 1e-8f) { *ox = x / l; *oy = y / l; } else { *ox = x; *oy = y; }
}

bool Place(const Foliage& f, int mi, int layer, float ax, float az, const float rect[4], const std::vector<Town>& towns, Mt& mt, FoliageGround& ground, std::vector<Placed>& out) {
    const FoliageMesh& m = f.meshes[mi];
    float px = ax, pz = az, py = 0.f;
    const float slope = ground.Slope(px, pz);
    if (slope > m.max_slope || slope < m.min_slope) return false;
    if (px < rect[0] || px > rect[2] || pz < rect[1] || pz > rect[3]) return true;   // outside the page (the zone)
    bool blocked = false;
    if (m.avoid_towns) {
        const int t = NearestTown(towns, px, pz);
        if (t >= 0) {
            const float dx = towns[t].x - px, dz = towns[t].z - pz;
            if (!m.building_type && dz * dz + dx * dx < towns[t].no_foliage * towns[t].no_foliage) blocked = true;
        }
    }
    py = ground.Height(px, pz);
    if (py > m.max_altitude || py < m.min_altitude) return false;
    if (m.floating) py = std::max(py, ground.WaterLevel());
    const float lo = std::min(std::max(m.min_height * 2.f, 0.01f), 10.f), hi = std::min(std::max(m.max_height * 2.f, 0.01f), 10.f);
    const float scale = mt.Range(lo, hi);
    float off = m.vertical_offset_min;
    if (off != m.vertical_offset_max) off = mt.Range(m.vertical_offset_min, m.vertical_offset_max);
    py += off * scale;
    if (m.road_avoidance > 0.f) {   // roads (the overlay's alpha) round the square of the road avoidance: its rim and middle
        const float r = m.road_avoidance * scale;
        int c0, r0, c1, r1; float cell;
        if (ground.OverlayCell(px - r, pz - r, &c0, &r0, &cell) && ground.OverlayCell(px + r, pz + r, &c1, &r1, &cell)) {
            auto road = [&](int c, int rr) { uint8_t px4[4]; return ground.OverlayAt(c, rr, px4) && px4[3] != 0; };
            for (int c = c0; c <= c1 && !blocked; ++c) if (road(c, r0)) blocked = true;
            if (!blocked && (c1 != c0 || r1 != r0)) {
                if (road((c0 + c1) / 2, (r0 + r1) / 2)) blocked = true;
                else if (r0 < r1) {
                    for (int c = c0; c <= c1 && !blocked; ++c) if (road(c, r1)) blocked = true;
                    for (int rr = r0 + 1; rr < r1 && !blocked; ++rr) if (road(c0, rr) || road(c1, rr)) blocked = true;
                }
            }
        }
    }
    const float rr = mt.Range(0.f, 360.f);   // the yaw, in 255 steps
    const float yaw = (float)(((uint32_t)(int64_t)((rr / 360.f) * 255.f)) & 0xffu) * (360.f / 255.f);
    if (!blocked && !m.building_type) out.push_back({ mi, layer, px, py, pz, yaw, scale });
    for (int ci : m.children) {   // child clusters round it
        const FoliageMesh& c = f.meshes[ci];
        const int clo = (int)c.cluster_num_min;
        const int n = (int)mt.Int((uint32_t)((int)c.cluster_num_max - clo)) + clo;
        const float radius = mt.Range(c.cluster_radius_min, c.cluster_radius_max);
        const float base = c.child_cluster_radius + m.child_cluster_radius;
        for (int k = 0; k < n; ++k) {
            const float a = mt.Range(-1.f, 1.f), b = mt.Range(-1.f, 1.f);
            float dx, dz; Normalise2(b, a, &dx, &dz);
            const float d = mt.Range(0.f, radius) + base;
            Place(f, ci, layer, px + dx * d, pz + dz * d, rect, towns, mt, ground, out);
        }
    }
    if (!blocked && m.grass_spot > 0.f) {   // a grass spot painted into the overlay's green over the pixels inside its square, the value compounding down by distance
        const float r = m.grass_spot * scale;
        int c0, r0, c1, r1; float cell, cell1;
        if (ground.OverlayCell(px - r, pz - r, &c0, &r0, &cell) && ground.OverlayCell(px + r, pz + r, &c1, &r1, &cell1)) {
            const bool several = c0 != c1 || r0 != r1;
            float v = std::min(std::max((r * 2.f) / cell, 0.3f), 1.f);
            for (int c = c0; c <= c1; ++c)
                for (int row = r0; row <= r1; ++row) {
                    float cx, cz;
                    if (!ground.OverlayCentre(c, row, &cx, &cz)) continue;
                    if (several) { const float d2 = (cx - px) * (cx - px) + (cz - pz) * (cz - pz); v *= 1.f - (d2 / (r * r)) * 0.3f; }
                    ground.SetOverlayGreen(c, row, (uint8_t)(int64_t)(v * 96.f));
                }
        }
    }
    return true;
}

} // namespace

void ScatterLayer(const Foliage& f, int layer, const float rect[4], int biome, const std::vector<Town>& towns, FoliageGround& ground, std::vector<Placed>& out) {
    const FoliageLayer& l = f.layers[layer];
    // the seed: the layer's id less a hash of the zone's corner
    uint32_t u = (uint32_t)((int32_t)rect[1] * 0x39 + (int32_t)rect[0]);
    u ^= u << 13;
    const uint32_t v = (u * u * 0xec4du + 0x0131071fu) * u + 0xd208dd0du;
    const uint32_t vv = v & 0x7fffffffu;
    const int32_t off = (int32_t)((double)vv * 9.313225746154785e-10 * 16777215.0 - 16777215.0);
    Mt mt((uint32_t)(l.seed_base - off));
    for (const auto& mp : l.meshes) {
        const int mi = mp.first;
        const FoliageMesh& m = f.meshes[mi];
        const int n = (int)mp.second;
        int remaining = 0; float radius = 0.f, cx = 0.f, cz = 0.f;
        int i = 0;
        while (i < n) {
            --remaining;
            bool skip = false;
            float px, pz;
            if (!m.clustered || remaining < 1) {
                if (m.clustered) {
                    const int lo = (int)m.cluster_num_min;
                    const uint32_t span = (uint32_t)((int)m.cluster_num_max - lo);
                    remaining = (int)mt.Int(span) + lo;
                    radius = mt.Range(m.cluster_radius_min, m.cluster_radius_max);
                }
                int tries = 0;   // a random place in the zone (on grass, if limited to it: 20 more tries)
                for (;;) {
                    const float x = mt.Range(rect[0], rect[2]), z = mt.Range(rect[1], rect[3]);
                    cx = x; cz = z;
                    if (!m.limit_to_grass) break;
                    uint8_t px4[4] = { 0, 0, 0, 0 };
                    ground.Overlay(x, z, px4);
                    if ((uint32_t)px4[1] + (uint32_t)px4[2] > 0x95u) break;
                    if (tries > 20) { remaining = 0; skip = true; break; }
                    ++tries;
                }
                px = cx; pz = cz;
            } else {
                const float a = mt.Range(-1.f, 1.f), b = mt.Range(-1.f, 1.f);
                float dx, dz; Normalise2(b, a, &dx, &dz);
                const float d = mt.Range(5.f, radius);
                px = cx + dx * d; pz = cz + dz * d;
            }
            if (!skip) {
                if (biome == kNoBiome || ground.BiomeIndex(px, pz) == biome) {
                    if (!Place(f, mi, layer, px, pz, rect, towns, mt, ground, out)) remaining = 0;
                } else {
                    if (remaining > 0) i += remaining;
                    remaining = 0;
                }
            }
            ++i;
        }
    }
}


namespace {

// Grass coverage noise: value noise, 5 octaves, cosine interpolation (Okran foliage.rs F041778 / F041780 / F041781), in double like the python stand-in tool.
double Lattice(int32_t x, int32_t y) {
    int32_t n = (int32_t)((uint32_t)x + (uint32_t)y * 0x39u);
    n = (int32_t)(((uint32_t)n << 13) ^ (uint32_t)n);
    const uint32_t nn = (uint32_t)n;
    const uint32_t v = (nn * (nn * nn * 0xec4du + 0x0131071fu) + 0xd208dd0du) & 0x7fffffffu;
    return 1.0 - (double)v * 9.313225746154785e-10;
}
double Smoothed(double x, double y) {
    const int32_t xi = (int32_t)x, yi = (int32_t)y, xm = (int32_t)(x - 1), xp = (int32_t)(x + 1), ym = (int32_t)(y - 1), yp = (int32_t)(y + 1);
    return (Lattice(xm, ym) + Lattice(xp, ym) + Lattice(xm, yp) + Lattice(xp, yp)) * 0.0625 + (Lattice(xm, yi) + Lattice(xp, yi) + Lattice(xi, ym) + Lattice(xi, yp)) * 0.125 + Lattice(xi, yi) * 0.25;
}
double Interpolated(double x, double y) {
    const double fx = std::floor(x), fy = std::floor(y);
    const double tx = (1 - std::cos((x - fx) * 3.1415927)) * 0.5, ty = (1 - std::cos((y - fy) * 3.1415927)) * 0.5;
    const double a = Smoothed(fx, fy), b = Smoothed(fx + 1, fy), c = Smoothed(fx, fy + 1), d = Smoothed(fx + 1, fy + 1);
    return (a * (1 - tx) + b * tx) * (1 - ty) + (c * (1 - tx) + d * tx) * ty;
}
double GrassNoise(double x, double z, double scale, double cutoff, double boost, double cap) {
    double s = 0;
    for (int i = 0; i < 5; ++i) { const double fr = (double)(1 << i); s += Interpolated(fr * x / scale, fr * z / scale) * std::pow(0.5, i); }
    const double n = s * 0.5 + 0.5;
    const double v = n <= cutoff ? 0.0 : (n - cutoff) / (1 - cutoff);
    return std::max(0.0, std::min(cap, v * boost));
}

constexpr float kCell = 36.f;   // one overlay pixel, Kenshi units
constexpr double kOriginShift = 147456.0;

struct OverlayTiles {   // new_overlay.<tx>.<ty>.png, 1024 x 1024 RGBA, cached for the process
    std::map<std::pair<int, int>, std::vector<uint8_t>> tiles;
    const std::vector<uint8_t>& Get(const std::string& dir, int tx, int ty) {
        auto it = tiles.find({ tx, ty });
        if (it != tiles.end()) return it->second;
        std::vector<uint8_t> px((size_t)1024 * 1024 * 4, 0);
        char path[512]; snprintf(path, sizeof(path), "%s/data/newland/land/overlaymaps/new_overlay.%d.%d.png", dir.c_str(), tx, ty);
        int w = 0, h = 0, n = 0;
        if (unsigned char* d = stbi_load(path, &w, &h, &n, 4)) { if (w == 1024 && h == 1024) std::copy(d, d + px.size(), px.begin()); stbi_image_free(d); }
        return tiles.emplace(std::make_pair(tx, ty), std::move(px)).first->second;
    }
};

class ZoneGround : public FoliageGround {
public:
    ZoneGround(const std::string& dir, int zx, int zz, OverlayTiles& tiles, const std::vector<uint8_t>& biomemap, std::string& err) : biomemap_(biomemap) {
        x0_ = (float)((zx - 32) * 4608); z0_ = (float)((zz - 32) * 4608);
        const int c0 = (int)std::lround(((double)(zx - 32) * 4608 + kOriginShift) / kCell), r0 = (int)std::lround(((double)(zz - 32) * 4608 + kOriginShift) / kCell);
        for (int ty = r0 / 1024; ty <= (r0 + 128) / 1024; ++ty)
            for (int tx = c0 / 1024; tx <= (c0 + 128) / 1024; ++tx) {
                const auto& t = tiles.Get(dir, tx, ty);
                const int ca = std::max(c0, tx * 1024), cb = std::min(c0 + 129, tx * 1024 + 1024), ra = std::max(r0, ty * 1024), rb = std::min(r0 + 129, ty * 1024 + 1024);
                for (int r = ra; r < rb; ++r) for (int c = ca; c < cb; ++c) std::copy(&t[((size_t)(r - ty * 1024) * 1024 + (c - tx * 1024)) * 4], &t[((size_t)(r - ty * 1024) * 1024 + (c - tx * 1024)) * 4] + 4, &ov_[((r - r0) * 129 + (c - c0)) * 4]);
            }
        for (int i = 0; i < 129 * 129; ++i) { ov_[i * 4] = 0; ov_[i * 4 + 1] = 0; }   // R (grass coverage) and G (spots) are cleared before placing
        md::tiff::Image img; std::string e;
        const int i0 = (int)std::lround((z0_ + kOriginShift) / 18.0), j0 = (int)std::lround((x0_ + kOriginShift) / 18.0);
        std::vector<uint16_t> w;
        if (!img.Open((dir + "/data/newland/land/fullmap.tif").c_str(), e) || !img.Window((uint32_t)j0, (uint32_t)i0, 129, 129, 2, w, e) || w.size() < 129 * 129) { err = "fullmap.tif: " + e; return; }
        for (int i = 0; i < 129 * 129; ++i) h_[i] = (double)w[i] * 9800.0 / 65535.0;
        ok_ = true;
    }
    bool ok() const { return ok_; }
    float X0() const { return x0_; }
    float Z0() const { return z0_; }
    int BiomeKey(float x, float z) const {   // biomemap: 1024 px, 288 units a pixel, origin -147456
        const int px = std::min(1023, std::max(0, (int)((x + kOriginShift) / 288))), py = std::min(1023, std::max(0, (int)((z + kOriginShift) / 288)));
        const uint8_t* p = &biomemap_[((size_t)py * 1024 + px) * 3];
        return (p[0] << 16) | (p[1] << 8) | p[2];
    }
    float Height(float x, float z) override {
        const double fx = std::min(128.0, std::max(0.0, (double)(x - x0_) / 36)), fz = std::min(128.0, std::max(0.0, (double)(z - z0_) / 36));
        const int j = std::min(127, (int)fx), i = std::min(127, (int)fz); const double tx = fx - j, tz = fz - i;
        return (float)((H(i, j) * (1 - tx) + H(i, j + 1) * tx) * (1 - tz) + (H(i + 1, j) * (1 - tx) + H(i + 1, j + 1) * tx) * tz);
    }
    float Slope(float x, float z) override {
        const int i = std::min(127, std::max(1, (int)((z - z0_) / 36))), j = std::min(127, std::max(0, (int)((x - x0_) / 36)));
        return (float)std::max(std::fabs(H(i, j) - H(i - 1, j)), std::fabs(H(i, j) - H(i, j + 1)));
    }
    int BiomeIndex(float x, float z) override { return BiomeKey(x, z); }
    bool Cell(float x, float z, int* c, int* r) const { const int cc = (int)std::floor((x - x0_) / 36), rr = (int)std::floor((z - z0_) / 36); if (cc < 0 || cc >= 129 || rr < 0 || rr >= 129) return false; *c = cc; *r = rr; return true; }
    bool Overlay(float x, float z, uint8_t px[4]) override { int c, r; if (!Cell(x, z, &c, &r)) return false; return OverlayAt(c, r, px); }
    bool OverlayCell(float x, float z, int* col, int* row, float* cell) override { if (!Cell(x, z, col, row)) return false; *cell = kCell; return true; }
    bool OverlayAt(int col, int row, uint8_t px[4]) override {   // memory order B, G, R, A
        if (col < 0 || col >= 129 || row < 0 || row >= 129) return false;
        const uint8_t* p = &ov_[(row * 129 + col) * 4]; px[0] = p[2]; px[1] = p[1]; px[2] = p[0]; px[3] = p[3]; return true;
    }
    bool OverlayCentre(int col, int row, float* x, float* z) override { if (!(col > 0 && col < 128 && row > 0 && row < 128)) return false; *x = x0_ + ((float)col + 0.5f) * 36.f; *z = z0_ + ((float)row + 0.5f) * 36.f; return true; }
    void SetOverlayGreen(int col, int row, uint8_t v) override { if (col >= 0 && col < 129 && row >= 0 && row < 129) ov_[(row * 129 + col) * 4 + 1] = v; }
    float WaterLevel() override { return 100.f; }
    void PaintGrass(const Grass& g, bool filter_biome, int biome) {
        const double ns = (double)g.noise_scale * 72, bs = (double)g.blackout_noise_scale * 72;
        for (int r = 0; r < 129; ++r)
            for (int c = 0; c < 129; ++c) {
                const float x = x0_ + ((float)c + 0.5f) * 36.f, z = z0_ + ((float)r + 0.5f) * 36.f;
                if (filter_biome && BiomeKey(x, z) != biome) continue;
                const double h = Height(x, z);
                double fade = h < g.min_altitude ? (h - (g.min_altitude - 300)) / 300 : (h > g.max_altitude ? ((g.max_altitude + 300) - h) / 300 : 1.0);
                fade = std::max(0.0, std::min(1.0, fade));
                uint8_t* p = &ov_[(r * 129 + c) * 4];
                if (p[3] > 10) continue;
                const double full = p[0] > 70 ? 0.0 : 255.0;
                const double cap = g.cap;
                double n = GrassNoise(x, z, ns, g.zero_cutoff, g.brightness_boost, cap);
                if (g.blackout) n *= GrassNoise(x + 50007.0, z + 50007.0, bs, g.blackout_zero_cutoff, g.brightness_boost, cap);
                const uint8_t v = (uint8_t)((int64_t)(n * full * fade) & 0xff);
                if (p[0] < v) p[0] = v;
            }
    }
private:
    double H(int i, int j) const { return h_[i * 129 + j]; }
    const std::vector<uint8_t>& biomemap_;
    float x0_ = 0, z0_ = 0;
    uint8_t ov_[129 * 129 * 4] = {};
    double h_[129 * 129] = {};
    bool ok_ = false;
};

OverlayTiles& SharedTiles() { static OverlayTiles t; return t; }
const std::vector<uint8_t>& SharedBiomemap(const std::string& kenshi_dir, std::string& err) {
    static std::vector<uint8_t> bm;
    if (bm.empty()) {
        int w = 0, h = 0, n = 0;
        unsigned char* d = stbi_load((kenshi_dir + "/data/newland/land/biomemap.png").c_str(), &w, &h, &n, 3);
        if (!d || w != 1024 || h != 1024) { err = "biomemap.png"; if (d) stbi_image_free(d); return bm; }
        bm.assign(d, d + (size_t)1024 * 1024 * 3); stbi_image_free(d);
    }
    return bm;
}

} // namespace

bool ScatterZone(const std::string& kenshi_dir, const Foliage& f, int zx, int zz, int visibility, const std::vector<Town>& towns, std::vector<Placed>& out, std::string& err, const std::vector<int>* only_biomes) {
    OverlayTiles& tiles = SharedTiles();
    const std::vector<uint8_t>& biomemap = SharedBiomemap(kenshi_dir, err);
    if (biomemap.empty()) return false;
    auto zg = std::make_unique<ZoneGround>(kenshi_dir, zx, zz, tiles, biomemap, err);
    if (!zg->ok()) return false;
    ZoneGround& z = *zg;
    const float rect[4] = { z.X0(), z.Z0(), z.X0() + 4608.f, z.Z0() + 4608.f };
    std::vector<int> present;   // biome colours of the zone (16 x 16 samples of the biomemap), in the order first seen, that are a BIOMES index
    for (int i = 0; i < 16; ++i)
        for (int j = 0; j < 16; ++j) {
            const int key = z.BiomeKey(rect[0] + ((float)i + 0.5f) * 288.f, rect[1] + ((float)j + 0.5f) * 288.f);
            if (f.biome_by_index.count(key) && (!only_biomes || std::find(only_biomes->begin(), only_biomes->end(), key) != only_biomes->end()) && std::find(present.begin(), present.end(), key) == present.end()) present.push_back(key);
        }
    bool need_grass = false;
    for (int c : present)
        for (int li : f.biome_layers.at(f.biome_by_index.at(c))) {
            const FoliageLayer& L = f.layers[li];
            if (L.visibility_range != visibility) continue;
            for (const auto& mp : L.meshes) { const FoliageMesh& m = f.meshes[mp.first]; if (m.limit_to_grass || m.grass_spot > 0.f) need_grass = true; }
        }
    if (need_grass)   // the grass layers first: the red channel the limit-to-grass layers read
        for (int c : present)
            for (int gi : f.biome_grass.at(f.biome_by_index.at(c))) z.PaintGrass(f.grass[gi], present.size() != 1, c);
    for (int c : present) {
        const std::string& bid = f.biome_by_index.at(c);
        for (int li : f.biome_layers.at(bid)) {
            if (f.layers[li].visibility_range != visibility) continue;
            const size_t first = out.size();
            ScatterLayer(f, li, rect, present.size() == 1 ? kNoBiome : c, towns, z, out);
            for (size_t k = first; k < out.size(); ++k) out[k].biome = c;
        }
    }
    return true;
}

bool ScatterGrass(const std::string& kenshi_dir, const Foliage& f, float cx, float cz, float radius, float density_mult, uint32_t seed, size_t max_blades, std::vector<Blade>& out, std::string& err) {
    OverlayTiles& tiles = SharedTiles();
    const std::vector<uint8_t>& biomemap = SharedBiomemap(kenshi_dir, err);
    if (biomemap.empty()) return false;
    constexpr double kZone = 4608.0;
    const int zx0 = std::max(0, (int)std::floor(((double)cx - radius) / kZone) + 32), zx1 = std::min(63, (int)std::floor(((double)cx + radius) / kZone) + 32);
    const int zz0 = std::max(0, (int)std::floor(((double)cz - radius) / kZone) + 32), zz1 = std::min(63, (int)std::floor(((double)cz + radius) / kZone) + 32);
    for (int zz = zz0; zz <= zz1; ++zz)
        for (int zx = zx0; zx <= zx1; ++zx) {
            ZoneGround z(kenshi_dir, zx, zz, tiles, biomemap, err);
            if (!z.ok()) return false;
            const float x0 = z.X0(), z0 = z.Z0();
            std::vector<int> present;
            for (int i = 0; i < 16; ++i)
                for (int j = 0; j < 16; ++j) {
                    const int key = z.BiomeKey(x0 + ((float)i + 0.5f) * 288.f, z0 + ((float)j + 0.5f) * 288.f);
                    if (f.biome_by_index.count(key) && std::find(present.begin(), present.end(), key) == present.end()) present.push_back(key);
                }
            for (int c : present) for (int gi : f.biome_grass.at(f.biome_by_index.at(c))) z.PaintGrass(f.grass[gi], present.size() != 1, c);
            for (int c : present)
                for (int gi : f.biome_blades.at(f.biome_by_index.at(c))) {
                    const Grass& g = f.grass[gi];
                    const double cand = (double)g.density * density_mult * 3.14159265358979 * (double)radius * radius;
                    const int n = (int)cand;   // every zone rolls the same candidates over the whole circle; the points outside the zone are skipped below
                    std::mt19937 zr(seed ^ (uint32_t)(zx * 73856093) ^ (uint32_t)(zz * 19349663) ^ (uint32_t)(gi * 83492791));
                    auto zu = [&](double a, double b) { return a + (b - a) * ((double)zr() / 4294967296.0); };
                    for (int k = 0; k < n && out.size() < max_blades; ++k) {
                        const double an = zu(0, 6.283185307179586), r = (double)radius * std::sqrt(zu(0, 1));
                        const float x = (float)((double)cx + r * std::cos(an)), zc = (float)((double)cz + r * std::sin(an));
                        if (x < x0 || x >= x0 + (float)kZone || zc < z0 || zc >= z0 + (float)kZone) continue;
                        if (present.size() != 1 && z.BiomeKey(x, zc) != c) continue;
                        uint8_t px[4]; if (!z.Overlay(x, zc, px)) continue;
                        const double dens = std::max((double)px[2], (double)px[1]) / 255.0;   // memory order B, G, R, A: R = grass coverage, G = spots
                        if (dens <= zu(0, 1)) continue;
                        if (z.Slope(x, zc) > g.max_slope) continue;
                        const float h = z.Height(x, zc);
                        if ((g.min_altitude != 0.f && h < g.min_altitude) || (g.max_altitude != 0.f && h > g.max_altitude)) continue;
                        const float size = (float)zu((double)g.min_height * 9.0, (double)g.max_height * 17.0) / 10.f;
                        out.push_back({ x, h, zc, size, (float)zu(0, 360), gi });
                    }
                }
        }
    return true;
}

} // namespace md::world

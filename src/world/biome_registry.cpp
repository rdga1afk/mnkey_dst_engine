#include <monkey_dust/world/biome_def.h>
#include <monkey_dust/platform/md_log.h>
#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <cstring>
#include <filesystem>
#include <monkey_dust/io/fcs_gamedata.h>
#include <monkey_dust/io/kenshi_dir.h>

// Text format (one directive per line):
//   tex_count <N>
//   tex <index>|<diffuse_path>|<normal_path>   ('|'-delimited: real Kenshi
//       filenames can contain spaces, e.g. "Smallish sharpGravel_DIF.dds" --
//       confirmed by direct crash repro: whitespace-split parsing silently
//       truncated that one path, InitFromDDSArray failed for ALL layers at
//       once as a result, and the renderer segfaulted on the first frame.)
//   biome_count <N>
//   biome <slug> <tex_base> <tex_slope> <tex_cliff> <tex_grass> <tex_dirt> <tex_road>
//               <fog_r> <fog_g> <fog_b> <sky_r> <sky_g> <sky_b>
//               <legend_r> <legend_g> <legend_b>
// Lines starting with '#' and blank lines are ignored. Slugs must not
// contain spaces (biome lines are still whitespace-split). This is a
// generic parser — the real biome data lives in a private file outside
// this repo (see the private generator that emits this format for the
// authoritative field-by-field rationale).

bool BiomeRegistry::LoadFromFile(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) {
        MD_LOG(MD_LOG_WARNING, "[BiomeRegistry] cannot open %s — biome data not loaded", path);
        return false;
    }
    std::string text;
    char buf[4096];
    size_t got;
    while ((got = fread(buf, 1, sizeof buf, f)) > 0) text.append(buf, got);
    fclose(f);
    return LoadFromText(text.c_str(), path);
}

bool BiomeRegistry::LoadFromText(const char* text, const char* path) {
    tex_count_   = 0;
    biome_count_ = 0;

    char line[512];
    const char* cur = text;
    while (*cur) {
        // one line at a time, truncated to the buffer like fgets did
        size_t n = strcspn(cur, "\n");
        size_t m = n < sizeof(line) - 2 ? n : sizeof(line) - 2;
        memcpy(line, cur, m);
        line[m] = '\n';
        line[m + 1] = '\0';
        cur += n + (cur[n] == '\n' ? 1 : 0);
        char* p = line;
        while (*p == ' ' || *p == '\t') ++p;
        if (*p == '#' || *p == '\n' || *p == '\r' || *p == '\0') continue;

        if (!strncmp(p, "tex_count", 9)) {
            continue; // informational only; arrays are fixed-size
        }
        if (!strncmp(p, "tex ", 4)) {
            // '|'-delimited (not whitespace/sscanf %s): real filenames can
            // contain spaces, see format-comment above for the crash this fixed.
            char* rest = p + 4;
            char* bar1 = strchr(rest, '|');
            char* bar2 = bar1 ? strchr(bar1 + 1, '|') : nullptr;
            if (bar1 && bar2) {
                *bar1 = '\0'; *bar2 = '\0';
                int idx = atoi(rest);
                char* dif = bar1 + 1;
                char* nml = bar2 + 1;
                for (char* c = nml; *c; ++c) if (*c == '\n' || *c == '\r') { *c = '\0'; break; }
                if (idx >= 0 && idx < MAX_TEXTURES) {
                    strncpy(tex_paths_[idx], dif, MAX_PATH_LEN - 1);
                    strncpy(nml_paths_[idx], nml, MAX_PATH_LEN - 1);
                    if (idx + 1 > tex_count_) tex_count_ = idx + 1;
                }
            }
            continue;
        }
        if (!strncmp(p, "biome_count", 11)) {
            continue;
        }
        if (!strncmp(p, "biome ", 6)) {
            if (biome_count_ >= MAX_BIOMES) continue;
            BiomeEntry& e = biomes_[biome_count_];
            char slug[MAX_SLUG_LEN] = {0};
            BiomeDef& d = e.def;
            int lr = 0, lg = 0, lb = 0;
            int consumed = 0;
            int n = sscanf(p + 6, "%47s %d %d %d %d %d %d %f %f %f %f %f %f %d %d %d%n",
                slug, &d.tex_base, &d.tex_slope, &d.tex_cliff,
                &d.tex_grass, &d.tex_dirt, &d.tex_road,
                &d.fog_r, &d.fog_g, &d.fog_b,
                &d.sky_horizon_r, &d.sky_horizon_g, &d.sky_horizon_b,
                &lr, &lg, &lb, &consumed);
            if (n == 16) {
                strncpy(e.slug, slug, MAX_SLUG_LEN - 1);
                e.legend_rgb[0] = (uint8_t)lr;
                e.legend_rgb[1] = (uint8_t)lg;
                e.legend_rgb[2] = (uint8_t)lb;
                d.biome_id = biome_count_;
                // Trailing per-layer tiling + slope-band fields (see this
                // file's format comment): tile_base_x/y tile_slope_x/y
                // tile_cliff_x/y tile_dirt_x/y tile_grass_x/y tile_road_x/y.
                // task #12 (2026-09-03): base/dirt/grass/road tiling kept
                // (was previously discarded except cliff, fields 5,6).
                // task #13 (2026-09-05): slope tiling (fields 3,4) now also
                // kept -- the slope layer is wired into
                // TS_ComputeGroundAlbedo (shaders/terrain_shading_common.glsl).
                // Missing (older biome_table.txt) -> BiomeDef's in-class
                // defaults (1.0, 1.0) stand, matching "no scale change".
                float tiling[12];
                int consumed2 = 0;
                int got = sscanf(p + 6 + consumed, "%f %f %f %f %f %f %f %f %f %f %f %f%n",
                    &tiling[0], &tiling[1], &tiling[2], &tiling[3], &tiling[4], &tiling[5],
                    &tiling[6], &tiling[7], &tiling[8], &tiling[9], &tiling[10], &tiling[11],
                    &consumed2);
                if (got == 12) {
                    d.tile_base_x  = tiling[0];
                    d.tile_base_y  = tiling[1];
                    d.tile_slope_x = tiling[2];
                    d.tile_slope_y = tiling[3];
                    d.cliff_tiling_x = tiling[4];
                    d.cliff_tiling_y = tiling[5];
                    d.tile_dirt_x  = tiling[6];
                    d.tile_dirt_y  = tiling[7];
                    d.tile_grass_x = tiling[8];
                    d.tile_grass_y = tiling[9];
                    d.tile_road_x  = tiling[10];
                    d.tile_road_y  = tiling[11];
                    // Slope-band block (6 floats): slope_min1/max1/fade1 =
                    // the SLOPE layer's weights.x band; slope_min2/max2/
                    // fade2 = the CLIFF layer's weights.y band (real
                    // terrainfp4.hlsl: weights = smoothstep(slopeMin-
                    // slopeBlend, slopeMin, slope) * smoothstep(slopeMax+
                    // slopeBlend, slopeMax, slope), slope=1-N.y). Both
                    // halves wired into TS_ComputeGroundAlbedo (task #13,
                    // 2026-09-05/06) -- slope_min/max/fade for the SLOPE
                    // layer, cliff_min/max/fade for the CLIFF layer
                    // (replacing its own prior single-sided TS_CLIFF_MIN/
                    // TS_CLIFF_BLEND constant; see BiomeDef's field
                    // comment for the known upper-edge risk this carries).
                    // Then brightness_fix (1 float, 19th trailing field
                    // overall). Missing (older biome_table.txt) -> BiomeDef's
                    // in-class default (1.0, no-op) stands.
                    float slope_band[6];
                    int consumed3 = 0;
                    int got2 = sscanf(p + 6 + consumed + consumed2,
                        "%f %f %f %f %f %f%n",
                        &slope_band[0], &slope_band[1], &slope_band[2],
                        &slope_band[3], &slope_band[4], &slope_band[5],
                        &consumed3);
                    if (got2 == 6) {
                        d.slope_min  = slope_band[0];
                        d.slope_max  = slope_band[1];
                        d.slope_fade = slope_band[2];
                        d.cliff_min  = slope_band[3];
                        d.cliff_max  = slope_band[4];
                        d.cliff_fade = slope_band[5];
                        float bf = 1.0f;
                        int consumed4 = 0;
                        if (sscanf(p + 6 + consumed + consumed2 + consumed3, "%f%n",
                                   &bf, &consumed4) == 1) {
                            d.brightness_fix = bf;
                            // Trailing distort_amplitude/distort_wavelength
                            // (2 floats, 21st/22nd fields overall -- "wavy
                            // cliff lines", see BiomeDef's field comment).
                            // Missing (older biome_table.txt) -> BiomeDef's
                            // in-class default (0.0, no-op) stands.
                            float da = 0.0f, dw = 0.0f;
                            int consumed5 = 0;
                            if (sscanf(p + 6 + consumed + consumed2 + consumed3 + consumed4,
                                       "%f %f%n", &da, &dw, &consumed5) == 2) {
                                d.distort_amplitude = da;
                                d.distort_wavelength = dw;
                                // Trailing (2026-10-03): 4 overlay mults, fade distance, ground colour rgb.
                                float ex[8];
                                if (sscanf(p + 6 + consumed + consumed2 + consumed3 + consumed4 + consumed5,
                                           "%f %f %f %f %f %f %f %f",
                                           &ex[0], &ex[1], &ex[2], &ex[3], &ex[4], &ex[5], &ex[6], &ex[7]) == 8) {
                                    d.overlay_mult_cliff = ex[0]; d.overlay_mult_grass = ex[1];
                                    d.overlay_mult_dirt  = ex[2]; d.overlay_mult_road  = ex[3];
                                    d.fade_distance = ex[4];
                                    d.distant_r = ex[5]; d.distant_g = ex[6]; d.distant_b = ex[7];
                                }
                            }
                        }
                    }
                }
                ++biome_count_;
            }
            continue;
        }
    }

    if (biome_count_ > 0) default_ = biomes_[0].def;
    fprintf(stdout, "[BiomeRegistry] loaded %d biomes, %d ground textures from %s\n",
            biome_count_, tex_count_, path);
    return biome_count_ > 0;
}

const BiomeDef& BiomeRegistry::ForZone(const char* zone_slug) const {
    if (!zone_slug || !zone_slug[0]) return default_;
    for (int i = 0; i < biome_count_; ++i)
        if (!strcmp(biomes_[i].slug, zone_slug)) return biomes_[i].def;
    return default_;
}

const BiomeDef& BiomeRegistry::ForIndex(int idx) const {
    if (idx < 0 || idx >= biome_count_) return default_;
    return biomes_[idx].def;
}

const BiomeDef& BiomeRegistry::ForColor(uint8_t r, uint8_t g, uint8_t b) const {
    if (biome_count_ == 0) return default_;
    int best = 0, best_d2 = 0x7FFFFFFF;
    for (int i = 0; i < biome_count_; ++i) {
        int dr = (int)r - (int)biomes_[i].legend_rgb[0];
        int dg = (int)g - (int)biomes_[i].legend_rgb[1];
        int db = (int)b - (int)biomes_[i].legend_rgb[2];
        int d2 = dr*dr + dg*dg + db*db;
        if (d2 < best_d2) { best_d2 = d2; best = i; }
    }
    return biomes_[best].def;
}

const char* BiomeRegistry::GroundTexPath(int idx) const {
    if (idx < 0 || idx >= tex_count_) return "";
    return tex_paths_[idx];
}

const char* BiomeRegistry::GroundNmlPath(int idx) const {
    if (idx < 0 || idx >= tex_count_) return "";
    return nml_paths_[idx];
}

// ── Biome table from the FCS BIOMES records ──────────────────────────────────────────────────────────────────────────────────────
// A port of private/md_gen_biome_table.py's table construction (same field mapping, same two-pass first-use texture order, same
// number formats), so that the text built here is the text the generator wrote; LoadFromText then parses both the same way.
// Load time only (std::string / std::vector / std::filesystem on purpose).
namespace {

// Two ground textures the real data refers to that do not exist under those names in the extracted texture set.
const char* SubstituteTexture(const std::string& fn) {
    static const struct { const char* from; const char* to; } T[] = {
        {"WadiGravel_DIF.dds", "WadiGravel_DIF_HI.dds"},
        {"grass_green-01_diffusespecular.dds", "Flat_Land_DIF.dds"},
        {"RockGravelMix2_DIF.dds", "ScarredRock_DIF.dds"},
    };
    for (const auto& t : T) if (fn == t.from) return t.to;
    return nullptr;
}

const std::string* PropStr(const md::fcs::Record& r, const char* key) {
    for (const auto& kv : r.files) if (kv.first == key) return &kv.second;
    for (const auto& kv : r.strings) if (kv.first == key) return &kv.second;
    return nullptr;
}

// numeric field: floats first, then ints (the generator's props dict held both)
bool PropNum(const md::fcs::Record& r, const char* key, double& out) {
    for (const auto& kv : r.floats) if (kv.first == key) { out = (double)kv.second; return true; }
    for (const auto& kv : r.ints) if (kv.first == key) { out = (double)kv.second; return true; }
    return false;
}
bool PropInt(const md::fcs::Record& r, const char* key, int32_t& out) {
    for (const auto& kv : r.ints) if (kv.first == key) { out = kv.second; return true; }
    for (const auto& kv : r.floats) if (kv.first == key) { out = (int32_t)kv.second; return true; }
    return false;
}
double PropF(const md::fcs::Record& r, const char* key, double def) { double v; return PropNum(r, key, v) ? v : def; }

// os.path.basename(v.strip().replace('\\', '/')), then the substitution table; "" when the field is empty/absent
std::string TexFilename(const md::fcs::Record& r, const char* field) {
    const std::string* v = PropStr(r, field);
    if (!v) return "";
    size_t a = 0, b = v->size();
    while (a < b && isspace((unsigned char)(*v)[a])) ++a;
    while (b > a && isspace((unsigned char)(*v)[b - 1])) --b;
    std::string s = v->substr(a, b - a);
    if (s.empty()) return "";
    for (char& c : s) if (c == '\\') c = '/';
    size_t slash = s.rfind('/');
    std::string fn = slash == std::string::npos ? s : s.substr(slash + 1);
    if (fn.empty()) return "";
    const char* sub = SubstituteTexture(fn);
    return sub ? sub : fn;
}

std::string ReplaceAll(std::string s, const std::string& from, const std::string& to) {
    for (size_t p = 0; (p = s.find(from, p)) != std::string::npos; p += to.size()) s.replace(p, from.size(), to);
    return s;
}

bool Exists(const std::string& path) { std::error_code ec; return std::filesystem::exists(path, ec); }

// Best-effort DIF -> NML file name; "" when none (the synthetic flat normal is used then)
std::string NormalFor(const std::string& dif, const std::string& dir) {
    std::string cands[2] = {ReplaceAll(dif, "_DIF", "_NML"), ReplaceAll(dif, "DIF", "NML")};
    for (const auto& c : cands) if (c != dif && Exists(dir + "/" + c)) return c;
    // legacy names (no DIF marker) ship <stem>_n.dds / <stem>_NML.dds; only DXT1 files fit the normal array
    size_t dot = dif.rfind('.');
    std::string stem = dot == std::string::npos ? dif : dif.substr(0, dot);
    std::string ext = dot == std::string::npos ? "" : dif.substr(dot);
    for (const char* suffix : {"_n", "_NML"}) {
        std::string c = stem + suffix + ext;
        FILE* f = fopen((dir + "/" + c).c_str(), "rb");
        if (!f) continue;
        char h[128] = {};
        size_t got = fread(h, 1, 128, f);
        fclose(f);
        if (got >= 88 && memcmp(h + 84, "DXT1", 4) == 0) return c;
    }
    return "";
}

std::string Slug(const std::string& name) {
    std::string o; bool us = false;
    for (unsigned char c : name) {
        char l = (c >= 'A' && c <= 'Z') ? (char)(c + 32) : (char)c;
        if ((l >= 'a' && l <= 'z') || (l >= '0' && l <= '9')) { if (us && !o.empty()) o += '_'; us = false; o += l; }
        else us = true;
    }
    return o.empty() ? "biome" : o;
}

void ColorRef(int32_t v, int& r, int& g, int& b) { r = (v >> 16) & 0xFF; g = (v >> 8) & 0xFF; b = v & 0xFF; }

std::string Fmt(const char* f, double v) { char b[64]; snprintf(b, sizeof b, f, v); return b; }

} // namespace

std::string BiomeRegistry::BuildTableFromFcs(const std::vector<md::fcs::Record>& records, const char* terrain_tex_dir) {
    const std::string dir = terrain_tex_dir ? terrain_tex_dir : "";
    std::vector<std::string> tex_list, nml_list;
    auto get_or_add = [&](const std::string& fn) -> int {
        for (size_t i = 0; i < tex_list.size(); ++i) if (tex_list[i] == fn) return (int)i;
        tex_list.push_back(fn);
        nml_list.push_back(NormalFor(fn, dir));
        return (int)tex_list.size() - 1;
    };
    std::vector<const md::fcs::Record*> biomes_raw;
    for (const auto& r : records) if (r.kind == md::fcs::ITEM_BIOMES) biomes_raw.push_back(&r);

    // pass 1: the master texture list in first-use order, base/slope/vertical/grass/dirt/road per biome until one slot is empty
    static const char* const kSlots[6] = {"texture base", "texture slope", "texture vertical", "texture grass", "texture dirt", "texture road"};
    // (the generator's first pass also reserved the NAMES of the biomes that had all six slots and an index, so the second pass
    // numbers those again as "<name>_2" -- reproduced because the slugs are part of the table)
    std::vector<std::string> seen;
    auto reserve_name = [&](const std::string& name) {
        std::string key = name;
        int n = 2;
        while (std::find(seen.begin(), seen.end(), key) != seen.end()) key = name + "_" + std::to_string(n++);
        seen.push_back(key);
        return key;
    };
    for (const auto* r : biomes_raw) {
        bool all_slots = true;
        for (int k = 0; k < 6; ++k) {
            std::string fn = TexFilename(*r, kSlots[k]);
            if (fn.empty()) { all_slots = false; break; }
            get_or_add(fn);
        }
        int32_t idx_val;
        if (all_slots && PropInt(*r, "index", idx_val)) reserve_name(r->name);
    }

    // pass 2: the biomes (an empty slope/vertical/grass/dirt falls back to base, road to dirt)
    std::string biome_lines;
    int biome_count = 0;
    for (const auto* rp : biomes_raw) {
        const md::fcs::Record& r = *rp;
        std::string base_fn = TexFilename(r, "texture base");
        if (base_fn.empty()) continue;
        auto resolve = [&](const char* field, int fallback) { std::string fn = TexFilename(r, field); return fn.empty() ? fallback : get_or_add(fn); };
        int base_idx = get_or_add(base_fn);
        int slope_idx = resolve("texture slope", base_idx);
        int cliff_idx = resolve("texture vertical", base_idx);
        int grass_idx = resolve("texture grass", base_idx);
        int dirt_idx = resolve("texture dirt", base_idx);
        int road_idx = resolve("texture road", dirt_idx);
        int32_t idx_val;
        if (!PropInt(r, "index", idx_val)) continue;
        int cr, cg, cb; ColorRef(idx_val, cr, cg, cb);
        int gr = cr, gg = cg, gb = cb;
        int32_t gv; bool has_ground = PropInt(r, "ground colour", gv);
        if (has_ground) ColorRef(gv, gr, gg, gb);
        std::string key = reserve_name(r.name);

        auto tile = [&](const char* kx, const char* ky) { return std::make_pair(PropF(r, kx, 1.0), PropF(r, ky, 1.0)); };
        auto base_t = tile("tiling X 0", "tiling Y 0"), slope_t = tile("tiling X 1", "tiling Y 1"), cliff_t = tile("tiling X 2", "tiling Y 2");
        auto dirt_t = tile("tiling X dirt", "tiling Y dirt"), grass_t = tile("tiling X grass", "tiling Y grass"), road_t = tile("tiling X road", "tiling Y road");
        double sb[3] = {PropF(r, "slope min 1", 15.0) / 100.0, PropF(r, "slope max 1", 55.0) / 100.0, PropF(r, "slope fade 1", 12.0) / 100.0};
        double cb2[3] = {PropF(r, "slope min 2", 50.0) / 100.0, PropF(r, "slope max 2", 100.0) / 100.0, PropF(r, "slope fade 2", 15.0) / 100.0};
        double brightness = PropF(r, "brightness fix", 1.0);
        double da = PropF(r, "distort amplitude", 0.0), dw = PropF(r, "distort wavelength", 1000.0);
        double om[4] = {PropF(r, "overlay mult vertical", 1.0), PropF(r, "overlay mult grass", 1.0), PropF(r, "overlay mult dirt", 1.0), PropF(r, "overlay mult road", 1.0)};
        double fade = PropF(r, "fade distance", 4000.0);
        int dr = 0xC0, dg = 0xA0, db = 0x40;
        if (has_ground) { dr = gr; dg = gg; db = gb; }

        std::string line = "biome " + Slug(key);
        for (int t : {base_idx, slope_idx, cliff_idx, grass_idx, dirt_idx, road_idx}) line += " " + std::to_string(t);
        line += " " + Fmt("%.3f", gr / 255.0 * 0.55) + " " + Fmt("%.3f", gg / 255.0 * 0.55) + " " + Fmt("%.3f", gb / 255.0 * 0.55);
        line += " " + Fmt("%.3f", gr / 255.0) + " " + Fmt("%.3f", gg / 255.0) + " " + Fmt("%.3f", gb / 255.0);
        line += " " + std::to_string(cr) + " " + std::to_string(cg) + " " + std::to_string(cb);
        for (const auto& pr : {base_t, slope_t, cliff_t, dirt_t, grass_t, road_t}) line += " " + Fmt("%.3f", pr.first) + " " + Fmt("%.3f", pr.second);
        for (double v : {sb[0], sb[1], sb[2], cb2[0], cb2[1], cb2[2]}) line += " " + Fmt("%.4f", v);
        line += " " + Fmt("%.3f", brightness) + " " + Fmt("%.4f", da) + " " + Fmt("%.4f", dw);
        for (double v : om) line += " " + Fmt("%.4f", v);
        line += " " + Fmt("%.1f", fade) + " " + Fmt("%.4f", dr / 255.0) + " " + Fmt("%.4f", dg / 255.0) + " " + Fmt("%.4f", db / 255.0);
        biome_lines += line + "\n";
        ++biome_count;
    }

    std::string out = "tex_count " + std::to_string(tex_list.size()) + "\n";
    for (size_t i = 0; i < tex_list.size(); ++i) {
        std::string nml = nml_list[i].empty() ? "tmp_/kenshi_re/terrain_textures/_MD_Flat_Normal_NML.dds" : dir + "/" + nml_list[i];
        out += "tex " + std::to_string(i) + "|" + dir + "/" + tex_list[i] + "|" + nml + "\n";
    }
    out += "biome_count " + std::to_string(biome_count) + "\n" + biome_lines;
    return out;
}

bool BiomeRegistry::LoadFromFcs(const std::vector<md::fcs::Record>& records, const char* terrain_tex_dir) {
    std::string text = BuildTableFromFcs(records, terrain_tex_dir);
    BiomeRegistry probe;
    if (!probe.LoadFromText(text.c_str(), "FCS BIOMES")) return false;
    return LoadFromText(text.c_str(), "FCS BIOMES");
}

std::string BiomeRegistry::ResolveKenshiDir() {
    return md::KenshiDir();
}

bool BiomeRegistry::LoadFromKenshi(const char* kenshi_dir, const char* terrain_tex_dir) {
    md::fcs::Install inst(kenshi_dir ? kenshi_dir : "");
    std::string why;
    if (!inst.Check(why, false)) { MD_LOG(MD_LOG_WARNING, "[BiomeRegistry] %s", why.c_str()); return false; }
    md::fcs::LoadOrder order; md::fcs::Error err;
    if (!inst.LoadOrderFor(order, err)) { MD_LOG(MD_LOG_WARNING, "[BiomeRegistry] %s", err.Message().c_str()); return false; }
    std::vector<md::fcs::Record> biomes;
    for (const auto& f : order.files) {
        md::fcs::File file;
        if (!md::fcs::ReadFile(f.path, file, err)) {
            if (f.core) { MD_LOG(MD_LOG_WARNING, "[BiomeRegistry] %s: %s", f.name.c_str(), err.Message().c_str()); return false; }
            MD_LOG(MD_LOG_WARNING, "[BiomeRegistry] mod %s skipped: %s", f.name.c_str(), err.Message().c_str());
            continue;
        }
        for (auto& r : file.records) if (r.kind == md::fcs::ITEM_BIOMES) biomes.push_back(std::move(r));
    }
    fprintf(stdout, "[BiomeRegistry] FCS: %zu BIOMES records from %zu files of %s\n", biomes.size(), order.files.size(), kenshi_dir);
    return LoadFromFcs(biomes, terrain_tex_dir);
}

bool BiomeRegistry::LoadPreferFcs(const char* fallback_table, const char* terrain_tex_dir) {
    std::string dir = ResolveKenshiDir();
    if (!dir.empty() && LoadFromKenshi(dir.c_str(), terrain_tex_dir)) return true;
    MD_LOG(MD_LOG_INFO, "[BiomeRegistry] no usable Kenshi FCS data (%s) -- loading %s", dir.empty() ? "none found" : dir.c_str(), fallback_table);
    return LoadFromFile(fallback_table);
}

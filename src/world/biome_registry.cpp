#include <monkey_dust/world/biome_def.h>
#include <monkey_dust/platform/md_log.h>
#include <cstdio>
#include <cstring>

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
    FILE* f = fopen(path, "r");
    if (!f) {
        MD_LOG(MD_LOG_WARNING, "[BiomeRegistry] cannot open %s — biome data not loaded", path);
        return false;
    }

    tex_count_   = 0;
    biome_count_ = 0;

    char line[512];
    while (fgets(line, sizeof(line), f)) {
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
                            if (sscanf(p + 6 + consumed + consumed2 + consumed3 + consumed4,
                                       "%f %f", &da, &dw) == 2) {
                                d.distort_amplitude = da;
                                d.distort_wavelength = dw;
                            }
                        }
                    }
                }
                ++biome_count_;
            }
            continue;
        }
    }
    fclose(f);

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

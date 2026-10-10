#include "terrain_gen_internal.h"
#include <monkey_dust/io/kenshi_dir.h>

// ── Biomemap colour lookup (real Kenshi biome-selection mechanism) ───────────
// game/data/textures/md_biomemap.png (1024x1024 RGB, converted 1:1 from
// Kenshi's real tmp_/kenshi/data/newland/land/biomemap.png). CONFIRMED this
// session (tools/md_gen_biome_table.py, not guessed): every real Kenshi
// "Biomes" FCS entry (gamedata.base itemType=28) has an 'index' field — a
// packed Windows COLORREF — that matches an exact pixel colour in this file
// for 10+ directly-verified named regions (desert, Ashlands, Canyonlands,
// Canyonlands Crater, ...). This replaces the old areasmap.tga-based
// nearest-NAMED-zone approximation (kept ~59 named zones only, guessed
// biome parameters by hand) with the real mechanism: sample this exact
// chunk's biomemap colour directly, nearest-match against the real per-biome
// FCS data via BiomeDef::ForColor — precise per-chunk, not per-named-zone.
static uint8_t* s_biomemap     = nullptr;
static int      s_biomemap_w   = 0, s_biomemap_h = 0;
static bool     s_biomemap_tried = false;

void s_load_biomemap() {
    if (s_biomemap_tried) return;
    s_biomemap_tried = true;
    int comp = 0;
    s_biomemap = stbi_load(md::KenshiLandFile("biomemap.png", "game/data/textures/md_biomemap.png").c_str(), &s_biomemap_w, &s_biomemap_h, &comp, 3);
    if (!s_biomemap)
        fprintf(stderr, "[TerrainGen] md_biomemap.png not found — biome lookup falls back to default\n");
}

// task terrain-dedup (2026-07-29): s_load_blendmap/s_blendmap_touch REMOVED
// (compiler-confirmed unused after removing their only caller, the dead
// per-chunk blend_layers[0] write) — existed solely to gate the now-removed
// TerrainRenderer draw pipeline's combo-alternate crossfade branch.

// zx,zy: 0..63 zone grid coords. Returns false (no data) if biomemap missing.
// Mode (most frequent colour) over the zone's whole pixel block, not a
// single centre-pixel sample — same noise-robustness rationale as the old
// areasmap lookup this replaces (stray anti-aliased pixels at painted
// region edges would otherwise flip an occasional chunk to the wrong biome).
static bool s_biomemap_color(int zx, int zy, uint8_t out[3]) {
    if (!s_biomemap) return false;
    float scale = (float)s_biomemap_w / 64.0f;
    int x0 = (int)(zx * scale), x1 = (int)((zx + 1) * scale);
    int y0 = (int)(zy * scale), y1 = (int)((zy + 1) * scale);
    if (x1 <= x0) x1 = x0 + 1;
    if (y1 <= y0) y1 = y0 + 1;
    if (x0 < 0) x0 = 0; if (x1 > s_biomemap_w) x1 = s_biomemap_w;
    if (y0 < 0) y0 = 0; if (y1 > s_biomemap_h) y1 = s_biomemap_h;
    if (x0 >= x1 || y0 >= y1) return false;

    static constexpr int MAX_DISTINCT = 32;
    uint8_t colors[MAX_DISTINCT][3];
    int counts[MAX_DISTINCT] = {};
    int n_distinct = 0;

    for (int py = y0; py < y1; ++py) {
        for (int px = x0; px < x1; ++px) {
            const uint8_t* p = s_biomemap + ((size_t)py * s_biomemap_w + px) * 3;
            int found = -1;
            for (int i = 0; i < n_distinct; ++i) {
                if (colors[i][0] == p[0] && colors[i][1] == p[1] && colors[i][2] == p[2]) { found = i; break; }
            }
            if (found < 0 && n_distinct < MAX_DISTINCT) {
                found = n_distinct++;
                colors[found][0] = p[0]; colors[found][1] = p[1]; colors[found][2] = p[2];
            }
            if (found >= 0) counts[found]++;
        }
    }
    if (n_distinct == 0) return false;

    int best = 0;
    for (int i = 1; i < n_distinct; ++i) if (counts[i] > counts[best]) best = i;
    out[0] = colors[best][0]; out[1] = colors[best][1]; out[2] = colors[best][2];
    return true;
}

// Factored out so it can also be called for a chunk's 4 grid-neighbours
// (biome crossfade target, see blend_layers in terrain_chunk.h).
static const BiomeDef& s_resolve_biome(int zx, int zy) {
    uint8_t col[3];
    const BiomeRegistry& biomes = BiomeRegistry::Get();
    if (s_biomemap_color(zx, zy, col))
        return biomes.ForColor(col[0], col[1], col[2]);
    return biomes.ForZone(nullptr);
}

const BiomeDef& TerrainGen_ResolveBiome(int zx, int zy) {
    return s_resolve_biome(zx, zy);
}

#include "terrain_gen_internal.h"

// ── Terrain Atlas ─────────────────────────────────────────────────────────────
// 2026-07-19: on-disk format switched from a single 260MB float32 .r32 blob
// to a raw-uint16 "base" (read-only, written offline by tools/tif_to_r32.py
// — matches Kenshi's own uint16 source precision exactly) plus a small
// sparse raw "_edits.r32" overlay file the editor's terrain brush writes to.
// A 16-bit PNG base was tried first (2.7x smaller, 50.6MB vs 136.3MB) but
// measured ~5x SLOWER to load in this exact engine (1.79s vs 0.35s,
// stb_image DEFLATE decode cost) — this project's target hardware (Intel
// HD 520) has a documented history of user-reported slow startup
// (CLAUDE.md task #158c), so load time won over disk size; raw uint16 is
// still under half the size of the original float32 .r32. The base is
// still never edited in-place (even though raw uint16 COULD support
// in-place zone writes, a sparse edits-overlay is simpler and keeps every
// brush stroke a small, fast append instead of a full-file rewrite). See
// TERRAIN_HEIGHT_SCALE_M's doc comment for the uint16<->metres mapping
// (identical to tools/tif_to_r32.py's / tools/md_hmap_io.py's).
static constexpr uint32_t ATLAS_R16_MAGIC   = 0x3631524Du; // "MR16" LE — matches tools/md_hmap_io.py's R16_MAGIC
static constexpr uint32_t ATLAS_EDITS_MAGIC = 0x4541444Du; // "MDAE" LE
static constexpr int      ATLAS_R16_SIZE = ATLAS_ZONES * ATLAS_VERTS; // 8256, r16 grid side
// Must match tools/tif_to_r32.py's/tools/md_hmap_io.py's HEIGHT_MAX_M
// exactly — this is the global uint16[0..65535] <-> metres[0..980] scale
// the raw-uint16 base is quantized with. Kenshi's own fullmap.tif source
// is ALREADY uint16 at this same effective precision (~0.015m/step), so
// this round-trip is lossless relative to the real source data, not a new
// quantization step.
static constexpr float    TERRAIN_HEIGHT_SCALE_M = 980.0f;

float s_atlas_h   [ATLAS_ZONES * ATLAS_ZONES * ATLAS_ZBLOCK]; // ~260 MB BSS
static float   s_atlas_hmin[ATLAS_ZONES * ATLAS_ZONES];
static float   s_atlas_hmax[ATLAS_ZONES * ATLAS_ZONES];
static uint8_t s_atlas_dirty[ATLAS_ZONES * ATLAS_ZONES]; // "ever edited this session" — NOT reset on save (see TerrainAtlas_Save)
bool    s_atlas_loaded = false;
static char    s_atlas_base_path[512] = {}; // base path with no extension, as passed by the caller

int s_atlas_zi(int zx, int zy)          { return zy * ATLAS_ZONES + zx; }
int s_atlas_hi(int zx, int zy, int c, int r) {
    return s_atlas_zi(zx, zy) * ATLAS_ZBLOCK + r * ATLAS_VERTS + c;
}

// Derives "<base>.r16" / "<base>_edits.r32" from the caller's base path.
// Callers pass a path with no extension (e.g. "game/data/terrain/world_hmap");
// tolerate an accidental trailing ".r32" from old call sites by stripping it.
static void s_atlas_derive_paths(const char* base, char* r16_out, char* edits_out, size_t out_sz) {
    char stripped[512];
    strncpy(stripped, base, sizeof(stripped) - 1);
    stripped[sizeof(stripped) - 1] = '\0';
    size_t len = strlen(stripped);
    if (len > 4 && strcmp(stripped + len - 4, ".r32") == 0) stripped[len - 4] = '\0';
    snprintf(r16_out,   out_sz, "%s.r16",       stripped);
    snprintf(edits_out, out_sz, "%s_edits.r32", stripped);
}

// Loads the read-only raw-uint16 base into s_atlas_h (tiled 8256x8256 =
// 64x64 zones of 129x129, exactly mirroring the old .r32's per-zone layout
// — see tools/tif_to_r32.py). hmin/hmax per zone are recomputed by
// scanning (the file carries no per-zone range, only the single global
// TERRAIN_HEIGHT_SCALE_M). 8-byte header (magic, zone count) — see
// tools/md_hmap_io.py's save_atlas_tiled for the writer.
static bool s_atlas_load_r16_base(const char* r16_path) {
    FILE* f = fopen(r16_path, "rb");
    if (!f) return false;
    uint32_t magic = 0, zones = 0;
    if (fread(&magic, 4, 1, f) != 1 || fread(&zones, 4, 1, f) != 1 ||
        magic != ATLAS_R16_MAGIC || zones != (uint32_t)ATLAS_ZONES) {
        fprintf(stderr, "[TerrainAtlas] %s: bad r16 header (magic=%#x zones=%u)\n", r16_path, magic, zones);
        fclose(f);
        return false;
    }
    std::vector<uint16_t> raw((size_t)ATLAS_R16_SIZE * ATLAS_R16_SIZE);
    bool ok = fread(raw.data(), 2, raw.size(), f) == raw.size();
    fclose(f);
    if (!ok) {
        fprintf(stderr, "[TerrainAtlas] %s: truncated (expected %zu uint16 samples)\n", r16_path, raw.size());
        return false;
    }
    for (int zy = 0; zy < ATLAS_ZONES; ++zy) {
        for (int zx = 0; zx < ATLAS_ZONES; ++zx) {
            int zi = s_atlas_zi(zx, zy);
            float hmin =  1e30f, hmax = -1e30f;
            for (int row = 0; row < ATLAS_VERTS; ++row) {
                const uint16_t* src_row = raw.data() + (size_t)(zy * ATLAS_VERTS + row) * ATLAS_R16_SIZE + zx * ATLAS_VERTS;
                float* dst_row = &s_atlas_h[s_atlas_hi(zx, zy, 0, row)];
                for (int col = 0; col < ATLAS_VERTS; ++col) {
                    float hgt = src_row[col] * (TERRAIN_HEIGHT_SCALE_M / 65535.0f);
                    dst_row[col] = hgt;
                    if (hgt < hmin) hmin = hgt;
                    if (hgt > hmax) hmax = hgt;
                }
            }
            s_atlas_hmin[zi] = hmin;
            s_atlas_hmax[zi] = hmax;
        }
    }
    return true;
}

// Overlays any previously-saved zone edits (sparse list) on top of the
// already-loaded PNG base. Missing file is expected/silent (no edits yet).
static void s_atlas_load_edits(const char* edits_path) {
    FILE* f = fopen(edits_path, "rb");
    if (!f) return;
    uint32_t magic = 0, n_zones = 0;
    if (fread(&magic, 4, 1, f) != 1 || magic != ATLAS_EDITS_MAGIC ||
        fread(&n_zones, 4, 1, f) != 1) {
        fclose(f); return;
    }
    uint32_t applied = 0;
    for (uint32_t i = 0; i < n_zones; ++i) {
        uint32_t zx = 0, zy = 0;
        float hmin = 0.f, hmax = 0.f;
        if (fread(&zx, 4, 1, f) != 1 || fread(&zy, 4, 1, f) != 1 ||
            fread(&hmin, 4, 1, f) != 1 || fread(&hmax, 4, 1, f) != 1 ||
            zx >= (uint32_t)ATLAS_ZONES || zy >= (uint32_t)ATLAS_ZONES) break;
        int zi = s_atlas_zi((int)zx, (int)zy);
        if (fread(&s_atlas_h[zi * ATLAS_ZBLOCK], 4, ATLAS_ZBLOCK, f) != (size_t)ATLAS_ZBLOCK) break;
        s_atlas_hmin[zi] = hmin;
        s_atlas_hmax[zi] = hmax;
        s_atlas_dirty[zi] = 1; // re-mark so a subsequent Save keeps carrying it forward
        ++applied;
    }
    fclose(f);
    fprintf(stdout, "[TerrainAtlas] applied %u edited zone(s) from %s\n", applied, edits_path);
}

bool TerrainAtlas_Load(const char* path) {
    char r16_path[512], edits_path[512];
    s_atlas_derive_paths(path, r16_path, edits_path, sizeof(r16_path));
    if (!s_atlas_load_r16_base(r16_path)) return false;
    memset(s_atlas_dirty, 0, sizeof(s_atlas_dirty));
    s_atlas_load_edits(edits_path);
    s_atlas_loaded = true;
    strncpy(s_atlas_base_path, path, sizeof(s_atlas_base_path) - 1);
    fprintf(stdout, "[TerrainAtlas] loaded %s\n", r16_path);
    return true;
}

bool TerrainAtlas_Loaded() { return s_atlas_loaded; }

float TerrainAtlas_GetHeight(int zx, int zy, int col, int row) {
    if (!s_atlas_loaded || zx < 0 || zx >= ATLAS_ZONES ||
        zy < 0 || zy >= ATLAS_ZONES) return 0.f;
    return s_atlas_h[s_atlas_hi(zx, zy, col, row)];
}

void TerrainAtlas_SetHeight(int zx, int zy, int col, int row, float h) {
    if (!s_atlas_loaded || zx < 0 || zx >= ATLAS_ZONES ||
        zy < 0 || zy >= ATLAS_ZONES) return;
    int idx = s_atlas_hi(zx, zy, col, row);
    s_atlas_h[idx] = h;
    int zi = s_atlas_zi(zx, zy);
    s_atlas_dirty[zi] = 1;
    if (h < s_atlas_hmin[zi]) s_atlas_hmin[zi] = h;
    if (h > s_atlas_hmax[zi]) s_atlas_hmax[zi] = h;
}

bool TerrainAtlas_ZoneDirty(int zx, int zy) {
    if (zx < 0 || zx >= ATLAS_ZONES || zy < 0 || zy >= ATLAS_ZONES) return false;
    return s_atlas_dirty[s_atlas_zi(zx, zy)] != 0;
}

bool TerrainAtlas_Save(const char* path) {
    if (!s_atlas_loaded) return false;
    char r16_path[512], edits_path[512];
    s_atlas_derive_paths(path, r16_path, edits_path, sizeof(r16_path));
    (void)r16_path; // base is read-only, never rewritten here
    FILE* f = fopen(edits_path, "wb");
    if (!f) return false;
    uint32_t n_dirty = 0;
    for (int i = 0; i < ATLAS_ZONES * ATLAS_ZONES; ++i) if (s_atlas_dirty[i]) ++n_dirty;
    uint32_t magic = ATLAS_EDITS_MAGIC;
    fwrite(&magic, 4, 1, f);
    fwrite(&n_dirty, 4, 1, f);
    for (int zy = 0; zy < ATLAS_ZONES; ++zy) {
        for (int zx = 0; zx < ATLAS_ZONES; ++zx) {
            int zi = s_atlas_zi(zx, zy);
            if (!s_atlas_dirty[zi]) continue;
            uint32_t uzx = (uint32_t)zx, uzy = (uint32_t)zy;
            fwrite(&uzx, 4, 1, f);
            fwrite(&uzy, 4, 1, f);
            fwrite(&s_atlas_hmin[zi], 4, 1, f);
            fwrite(&s_atlas_hmax[zi], 4, 1, f);
            fwrite(&s_atlas_h[zi * ATLAS_ZBLOCK], 4, ATLAS_ZBLOCK, f);
        }
    }
    fclose(f);
    fprintf(stdout, "[TerrainAtlas] saved %u edited zone(s) to %s\n", n_dirty, edits_path);
    return true;
}

// SaveEdits/LoadEdits/HasEdits — thin wrappers over the atlas API
// (editor brush tool calls these)
bool TerrainAtlas_SaveEdits(const char* path) { return TerrainAtlas_Save(path); }
bool TerrainAtlas_LoadEdits(const char*)       { return s_atlas_loaded; } // already loaded by TerrainAtlas_Load
bool TerrainAtlas_HasEdits() {
    for (int i = 0; i < ATLAS_ZONES * ATLAS_ZONES; ++i)
        if (s_atlas_dirty[i]) return true;
    return false;
}

// World-space bilinear height sample across the whole 64×64 atlas (wx,wz in
// metres, [0, ATLAS_ZONES*CHUNK_SIZE)). Replaces TerrainMaster_SampleWorld
// for ground-snapping call sites now that the macro-geography layer is gone —
// this samples the REAL Kenshi zone data directly, at full atlas resolution.
float TerrainAtlas_SampleWorld(float wx, float wz) {
    if (!s_atlas_loaded) return 0.f;
    constexpr int GRID_N = ATLAS_ZONES * (ATLAS_VERTS - 1); // 8192 spans, 8193 verts/axis
    const float world_ext = (float)ATLAS_ZONES * CHUNK_SIZE;
    float u = wx / world_ext, v = wz / world_ext;
    u = u < 0.f ? 0.f : (u > 1.f ? 1.f : u);
    v = v < 0.f ? 0.f : (v > 1.f ? 1.f : v);

    float gx = u * GRID_N, gz = v * GRID_N;
    int gx0 = (int)gx; if (gx0 >= GRID_N) gx0 = GRID_N - 1;
    int gz0 = (int)gz; if (gz0 >= GRID_N) gz0 = GRID_N - 1;
    float tx = gx - gx0, tz = gz - gz0;

    auto sample_grid = [](int gcol, int grow) -> float {
        int zx = gcol / (ATLAS_VERTS - 1), col = gcol % (ATLAS_VERTS - 1);
        int zy = grow / (ATLAS_VERTS - 1), row = grow % (ATLAS_VERTS - 1);
        if (zx >= ATLAS_ZONES) { zx = ATLAS_ZONES - 1; col = ATLAS_VERTS - 1; }
        if (zy >= ATLAS_ZONES) { zy = ATLAS_ZONES - 1; row = ATLAS_VERTS - 1; }
        return s_atlas_h[s_atlas_hi(zx, zy, col, row)];
    };
    float h00 = sample_grid(gx0,     gz0);
    float h10 = sample_grid(gx0 + 1, gz0);
    float h01 = sample_grid(gx0,     gz0 + 1);
    float h11 = sample_grid(gx0 + 1, gz0 + 1);
    return (h00 * (1.f - tx) + h10 * tx) * (1.f - tz)
         + (h01 * (1.f - tx) + h11 * tx) * tz;
}

// TerrainAtlas_IsWalkableWorld REMOVED (physical-design refactor Phase -1,
// 2026-09-17) -- its only caller, TerrainQuery::IsWalkable(), itself had
// zero external callers anywhere in engine/game/tools.

#include "terrain_gen_internal.h"
#include <mutex>

// ── Height sampling helpers ────────────────────────────────────────────────────

static float s_gen_height(int col, int row, ChunkCoord coord, const TerrainGenParams& p) {
    float wx = (coord.x * CHUNK_SIZE) + col * TERRAIN_STEP;
    float wz = (coord.z * CHUNK_SIZE) + row * TERRAIN_STEP;

    // ── Noise detail layer ────────────────────────────────────────────────────
    float sx = wx * p.base_scale + p.seed * 127.1f;
    float sz = wz * p.base_scale + p.seed * 311.7f;

    if (p.domain_warp_strength > 0.f) {
        float ws = p.base_scale * p.domain_warp_strength;
        sx += SimplexNoise2(sx + 1.7f, sz + 9.2f) * ws;
        sz += SimplexNoise2(sx + 8.3f, sz + 2.8f) * ws;
    }

    float base = (FBM2(sx, sz, p.octaves, p.persistence, p.lacunarity) + 1.f) * 0.5f;

    if (p.redistribution_power != 1.f)
        base = powf(base, p.redistribution_power);

    float h = base * p.amplitude;

    if (p.ridge_weight > 0.f) {
        // Ridged multifractal: 3 octaves, signal-dependent weighting.
        // Each octave is (1-|s|)² weighted by the previous octave's value
        // → high-freq detail appears only near ridge peaks, not in valleys.
        // Produces irregular, jagged ridges instead of smooth mathematical triangles.
        float rx = wx * p.base_scale * 0.5f + p.seed * 73.1f;
        float rz = wz * p.base_scale * 0.5f + p.seed * 149.3f;
        float ridge = 0.f, ra = 0.625f, prev = 1.f;
        for (int ro = 0; ro < 3; ++ro) {
            float s = 1.f - fabsf(SimplexNoise2(rx, rz));
            s = s * s * prev;
            ridge += s * ra;
            prev  =  s;
            ra   *= 0.5f;
            rx   *= 2.17f;  // slightly off 2.0 → breaks repetitive symmetry
            rz   *= 2.17f;
        }
        h += ridge * p.amplitude * p.ridge_weight;
    }

    return h < p.sea_level ? p.sea_level : h;
}

static inline int s_idx(int col, int row) { return row * (TERRAIN_GRID + 1) + col; }

// ── TerrainGen_Build ──────────────────────────────────────────────────────────

// task terrain-dedup (2026-07-29): s_verts_buf/s_idx_buf/s_skirt_v/s_skirt_i
// (fed only the now-removed chunk.vbo/ibo/skirt_vbo/skirt_ibo GPU buffers)
// and s_nav_pos/s_nav_tri (computed but never exposed via any accessor —
// per-chunk Recast navmesh has been disabled since before this session,
// NPC pathfinding uses the NavSystem singleton instead, see this
// function's own "4. NavMesh (disabled)" comment below) all REMOVED —
// verified zero readers, same investigation as terrain_renderer.h's class
// doc comment.

// See terrain_gen.h's doc comment on TerrainGen_StagingMutex(): guards the
// buffers above across the main thread's synchronous Build+Upload fallback
// (HandleTerrainStreaming/HandleFlythroughStreaming) and TerrainStreamQueue's
// worker thread, which were previously unsynchronized against each other.
std::mutex& TerrainGen_StagingMutex() {
    static std::mutex m;
    return m;
}

// Load r32 heightmap file into a flat buffer (width*height floats).
// Returns false on error. Caller owns buffer (use delete[]).
static bool s_load_r32(const char* path, float*& buf_out, int& w_out, int& h_out,
                        float& hmin_out, float& hmax_out) {
    FILE* f = fopen(path, "rb");
    if (!f) return false;
    unsigned int w, h;
    float hmin, hmax;
    if (fread(&w, 4, 1, f) != 1 || fread(&h, 4, 1, f) != 1 ||
        fread(&hmin, 4, 1, f) != 1 || fread(&hmax, 4, 1, f) != 1) {
        fclose(f); return false;
    }
    buf_out  = new float[w * h];
    w_out    = (int)w; h_out = (int)h;
    hmin_out = hmin;   hmax_out = hmax;
    bool ok  = fread(buf_out, sizeof(float), w * h, f) == w * h;
    fclose(f);
    if (!ok) { delete[] buf_out; buf_out = nullptr; return false; }
    return true;
}

static float s_hmap_sample(const float* hmap, int hmap_w, int hmap_h,
                             float u, float v) {
    // Bilinear sample; u,v in [0,1]
    float fx = u * (hmap_w - 1);
    float fz = v * (hmap_h - 1);
    int   x0 = (int)fx, z0 = (int)fz;
    int   x1 = x0 + 1 < hmap_w ? x0 + 1 : x0;
    int   z1 = z0 + 1 < hmap_h ? z0 + 1 : z0;
    float tx = fx - x0, tz = fz - z0;
    float h00 = hmap[z0 * hmap_w + x0];
    float h10 = hmap[z0 * hmap_w + x1];
    float h01 = hmap[z1 * hmap_w + x0];
    float h11 = hmap[z1 * hmap_w + x1];
    float h0  = h00 + tx * (h10 - h00);
    float h1  = h01 + tx * (h11 - h01);
    return h0 + tz * (h1 - h0);
}

bool TerrainGen_Build(TerrainChunk& out, ChunkCoord coord, const TerrainGenParams& p) {
    // Task terrain-patches (milder artifact, 2026-07-17): out.loaded must go
    // false BEFORE any other field on `out` is touched, not after. Chunk array
    // slots are reused across streaming, and this is the SAME live TerrainChunk
    // sitting in the streaming grid (worker thread calls Build directly on it,
    // no private copy) — TerrainQuery::GetHeight() and other callers trust
    // `chunk.loaded` as their ONLY gate before reading chunk.heightmap.h[]
    // directly. Setting loaded=false only near the end (after heights/verts/
    // skirts were already overwritten in place) left loaded==true (stale, from
    // the previous occupant) for nearly this whole function's runtime, so a
    // concurrent GetHeight() call during that window could read a torn mix of
    // old-location and new-location heights for this chunk.
    out.loaded = false;

    // ── 1. Heights ────────────────────────────────────────────────────────────
    if (!p.force_noise && p.zone_origin_x >= 0) {
        int zx = p.zone_origin_x + coord.x;
        int zy = p.zone_origin_z + coord.z;
        // Real Kenshi zone data — always in RAM once TerrainAtlas_Load() has
        // run at startup, and every real gameplay chunk request resolves to
        // an in-bounds (zx,zy). Direct copy, no file I/O per chunk.
        const float* src = &s_atlas_h[s_atlas_zi(zx, zy) * ATLAS_ZBLOCK];
        for (int row = 0; row <= TERRAIN_GRID; ++row)
            for (int col = 0; col <= TERRAIN_GRID; ++col)
                out.heightmap.h[s_idx(col, row)] = src[row * ATLAS_VERTS + col];
    } else if (!p.force_noise && p.heightmap_r32) {
        // Load Kenshi heightmap and sample heights for this chunk
        static const float* s_hmap     = nullptr;
        static int          s_hmap_w   = 0, s_hmap_h = 0;
        static float        s_hmap_range = 1.0f;
        static const char*  s_hmap_path = nullptr;

        if (s_hmap_path != p.heightmap_r32) {
            delete[] s_hmap;
            float hmin, hmax;
            float* tmp = nullptr;
            if (s_load_r32(p.heightmap_r32, tmp, s_hmap_w, s_hmap_h, hmin, hmax)) {
                s_hmap      = tmp;
                s_hmap_range = (hmax - hmin > 1e-3f) ? (hmax - hmin) : 1.0f;
                s_hmap_path  = p.heightmap_r32;
            } else {
                s_hmap = nullptr; s_hmap_path = nullptr;
            }
        }

        float total_size = 7 * CHUNK_SIZE;  // legacy single-file mode: 7×7 grid
        for (int row = 0; row <= TERRAIN_GRID; ++row) {
            for (int col = 0; col <= TERRAIN_GRID; ++col) {
                float wx  = (coord.x * CHUNK_SIZE + col * TERRAIN_STEP) + p.world_offset_x;
                float wz  = (coord.z * CHUNK_SIZE + row * TERRAIN_STEP) + p.world_offset_z;
                float u   = (wx - p.world_offset_x + total_size * 0.5f) / total_size;
                float v   = (wz - p.world_offset_z + total_size * 0.5f) / total_size;
                u = u < 0.f ? 0.f : (u > 1.f ? 1.f : u);
                v = v < 0.f ? 0.f : (v > 1.f ? 1.f : v);
                float raw = s_hmap ? s_hmap_sample(s_hmap, s_hmap_w, s_hmap_h, u, v) : 0.f;
                // Normalize raw to [-1,1] then scale by amplitude
                float h = (raw / s_hmap_range) * p.amplitude;
                out.heightmap.h[s_idx(col, row)] = h;
            }
        }
    } else {
        for (int row = 0; row <= TERRAIN_GRID; ++row) {
            for (int col = 0; col <= TERRAIN_GRID; ++col) {
                out.heightmap.h[s_idx(col, row)] = s_gen_height(col, row, coord, p);
            }
        }
    }
    // (zone_origin_x >= 0 branch closes above via its own else-if)

    // task terrain-dedup (2026-07-29): removed here (all verified zero
    // readers, same investigation as terrain_renderer.h class doc comment):
    // vertex/index building (fed only chunk.vbo/ibo, now removed), cross-
    // chunk normal stitching + geomorph targets (fed only s_verts_buf
    // fields uploaded to the same dead vbo), per-LOD-tier lod_error
    // (out.lod_error field removed), and skirt geometry (fed only
    // chunk.skirt_vbo/skirt_ibo, also removed). heights (step 1 above) and
    // pass_grid/coord/center (below) are unaffected -- those are the parts
    // physics/TerrainQuery/AI actually read.
    out.coord    = coord;
    out.center_x = p.world_offset_x + coord.x * CHUNK_SIZE + CHUNK_SIZE * 0.5f;
    out.center_z = p.world_offset_z + coord.z * CHUNK_SIZE + CHUNK_SIZE * 0.5f;
    // out.loaded already set false at function entry — Upload() sets it true.

    // Ensures md_biomemap.png is loaded (lazy, tried-once) for external
    // TerrainGen_ResolveBiome() callers (e.g. editor's s_build_zone_
    // ground_layers -- feeds Granite's live zoneGroundLayers SSBO). The
    // per-chunk ground_layers/blend_layers writes that used to follow here
    // were removed (task terrain-dedup, 2026-07-29) -- those fields fed
    // only the now-removed TerrainRenderer draw pipeline; s_load_blendmap/
    // s_blendmap_touch existed solely to compute blend_layers[0], now gone
    // too. Per-pixel ground selection in Granite's terrain_patch.frag reads
    // the SSBO directly, no per-chunk biome data needed here.
    if (p.zone_origin_x >= 0) s_load_biomemap();

    // ── 4. NavMesh (disabled) ──────────────────────────────────────────────
    // Per-chunk NavMesh disabled: NPC pathfinding uses NavSystem singleton.
    // Building 256×256 per-chunk navmeshes costs 15+ seconds at startup.
    (void)p.nav_cs; (void)p.nav_ch;

    // PassGrid (TerrainPassGrid, L2-inspired slope-threshold walkability)
    // population REMOVED (physical-design refactor Phase -1, 2026-09-17) —
    // its only reader, TerrainQuery::IsWalkable(), had zero external
    // callers itself, confirmed by grep before deletion.
    out.heightmap_ready = true;
    return true;
}

// task terrain-dedup (2026-07-29): TerrainGen_StagedVerts/StagedIndices/
// StagedSkirtVerts/StagedSkirtIndices REMOVED — their only caller
// (terrain_upload.cpp's s_upload_core) no longer needs staged mesh data,
// see that file's own updated comment.

// ── TerrainChunk::SampleHeight ────────────────────────────────────────────────

float TerrainChunk::SampleHeight(float lx, float lz) const {
    // Clamp to chunk bounds
    if (lx < 0.0f) lx = 0.0f;
    if (lz < 0.0f) lz = 0.0f;
    if (lx > CHUNK_SIZE) lx = CHUNK_SIZE;
    if (lz > CHUNK_SIZE) lz = CHUNK_SIZE;

    float fc = lx / TERRAIN_STEP;
    float fr = lz / TERRAIN_STEP;
    int c0 = (int)fc, r0 = (int)fr;
    if (c0 >= TERRAIN_GRID) c0 = TERRAIN_GRID - 1;
    if (r0 >= TERRAIN_GRID) r0 = TERRAIN_GRID - 1;
    int c1 = c0 + 1, r1 = r0 + 1;

    float tx = fc - c0, tz = fr - r0;
    float h00 = heightmap.h[s_idx(c0, r0)];
    float h10 = heightmap.h[s_idx(c1, r0)];
    float h01 = heightmap.h[s_idx(c0, r1)];
    float h11 = heightmap.h[s_idx(c1, r1)];
    return h00 * (1-tx)*(1-tz) + h10 * tx*(1-tz) + h01 * (1-tx)*tz + h11 * tx*tz;
}

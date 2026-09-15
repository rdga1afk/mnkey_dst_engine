#pragma once
#include <monkey_dust/world/terrain_chunk.h>
#include <monkey_dust/world/biome_def.h>
#include <mutex>

// Generates a TerrainChunk (mesh + heightmap + navmesh) from params.
// Call on a worker thread; GpuStaticBuffer::Init() must be deferred to main thread.
// Returns false on allocation failure (navmesh OOM).
bool TerrainGen_Build(TerrainChunk& out, ChunkCoord coord, const TerrainGenParams& p);

// Task terrain-patches: TerrainGen_Build writes into shared static staging
// buffers (s_verts_buf/s_idx_buf/etc, terrain_gen.cpp) — TerrainGen_UploadFrom's
// own doc comment already notes this "avoids race when worker thread reuses
// staging" for the WORKER THREAD's own sequential calls (TerrainStreamQueue's
// worker_loop processes one chunk per iteration by design). It does NOT cover
// the separate hazard confirmed live (task terrain-patches, CPU-side edge-
// length reconstruction from chunk.heightmap.h showed only normal ~3-30m
// edges everywhere a rendered chunk showed degenerate garbage triangles):
// the MAIN THREAD's synchronous fallback path (HandleTerrainStreaming/
// HandleFlythroughStreaming, main.cpp, used whenever TerrainStreamQueue's
// enqueue() reports the queue full) calls TerrainGen_Build directly with NO
// synchronization against the worker thread doing the same for a different
// chunk at the same moment — both write the SAME global buffers. Every
// caller of TerrainGen_Build (or the paired Build+consume-staging sequence)
// MUST hold this lock for the whole critical section, not just around Build
// itself, since the staged data remains valid only until the NEXT Build call
// on either thread.
std::mutex& TerrainGen_StagingMutex();

// Resolve the biome for a zone (zx,zy in 0..63) by sampling md_biomemap.png —
// same resolution used internally for per-chunk ground_layers (s_resolve_biome).
// Exposed so callers outside terrain_gen.cpp (e.g. the World3D editor's
// synthesis/compact-LOD background meshes) can build their own per-zone
// lookup tables without duplicating the biomemap-sampling logic.
const BiomeDef& TerrainGen_ResolveBiome(int zx, int zy);

// task terrain-dedup (2026-07-29): TerrainGen_Upload now only marks
// chunk.loaded=true — the GPU mesh buffers it used to build (vbo/ibo/
// ibo_lod/skirt_vbo/skirt_ibo/steepness_ssbo) were removed, verified zero
// readers (TerrainPatchRenderer/Granite is the sole active renderer and
// reads its own world-wide heightmap texture instead). Kept as a function
// (not inlined at call sites) since dozens of call sites pair it with
// TerrainGen_Build and a future per-chunk GPU resource may need it again.
// Implemented in engine/src/render/terrain_upload.cpp (still a separate,
// GPU-aware TU for that reason, even though it's now tiny).
void TerrainGen_Upload(TerrainChunk& chunk);

// Async upload path (Item 5): same as TerrainGen_Upload now (no staged
// buffers left to select between shared-static vs caller-provided) — kept
// as a distinct overload so existing call sites don't need touching.
void TerrainGen_UploadFrom(TerrainChunk& chunk,
                           const TerrainVertex* verts,
                           const uint16_t*      idx,
                           const TerrainVertex* skirt_v,
                           const uint16_t*      skirt_i);

// Simplex noise — public domain (Stefan Gustavson / Ashima Arts).
// grad4[] and permutation tables are file-internal; do not expose.
float SimplexNoise2(float x, float y);

// Fractal Brownian Motion over SimplexNoise2.
float FBM2(float x, float y, int octaves, float persistence, float lacunarity);

// ── Terrain Atlas API ────────────────────────────────────────────────────────
// world_hmap.r16: raw uint16 base (8256×8256, tools/tif_to_r32.py) + sparse
// "<base>_edits.r32" overlay for editor-brush changes. 64×64 zones, 129×129
// verts/zone (row=Z, col=X). See engine/src/world/terrain_gen.cpp for the
// exact on-disk layout (ATLAS_R16_MAGIC / ATLAS_EDITS_MAGIC).

bool  TerrainAtlas_Load(const char* path);  // load into static RAM buffer
bool  TerrainAtlas_Loaded();                // true after successful Load
// Smooth atlas heights at every zone boundary (3-vertex kernel) to eliminate
// Kenshi fullmap zone-seam cliffs that create NdotL=0 faces at chunk edges.
// Call once after Load, before any TerrainGen_Build.
void  TerrainAtlas_SmoothBoundaries();
// Stitch one boundary seam between adjacent zones after rolling-shift loads a new edge.
// dir=0: X-seam between (zx,zy) and (zx+1,zy). dir=1: Z-seam (zx,zy) and (zx,zy+1).
// No-op if already smoothed (idempotent — re-smoothing a smooth boundary is harmless).
void  TerrainAtlas_StitchEdge(int zx, int zy, int dir);
// Get/Set individual vertex height (zx,zy in 0..63; col,row in 0..64)
float TerrainAtlas_GetHeight(int zx, int zy, int col, int row);
void  TerrainAtlas_SetHeight(int zx, int zy, int col, int row, float h);
// Save only dirty zones back to the atlas file (fast partial update)
bool  TerrainAtlas_Save(const char* path);
bool  TerrainAtlas_ZoneDirty(int zx, int zy);

// Delta-edit layer (on top of read-only TIF mmap).
// SaveEdits writes only edited zones; LoadEdits applies them at startup.
bool  TerrainAtlas_SaveEdits(const char* path);
bool  TerrainAtlas_LoadEdits(const char* path);
bool  TerrainAtlas_HasEdits();

// World-space bilinear height sample across the full 64×64 atlas (wx,wz in
// metres, [0, 64*CHUNK_SIZE)=29491.2m). For ground-snapping/height-query
// call sites (editor camera, prop placement) — samples the REAL Kenshi zone
// data directly, at full atlas resolution.
float TerrainAtlas_SampleWorld(float wx, float wz);

// TerrainAtlas_IsWalkableWorld REMOVED (physical-design refactor Phase -1,
// 2026-09-17) -- its only caller, TerrainQuery::IsWalkable(), itself had
// zero external callers anywhere in engine/game/tools.

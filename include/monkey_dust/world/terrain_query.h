#pragma once
// TerrainQuery — single source of truth for terrain spatial data.
//
// Every system that needs to know where the ground is uses this.
//
// Rule: Y positions for ALL entities and props must come from here, never
// from TerrainChunk::SampleHeight() called ad-hoc in scattered places.
//
// 2026-07-26 (quadtree-LOD rewrite Phase 5, see plan at
// /home/rdga1/.claude/plans/serene-pondering-teapot.md): GetHeight/GetNormal
// /GetSlope/IsWater now read directly from TerrainAtlas_SampleWorld (the
// full-world, always-resident CPU heightmap, terrain_gen.cpp) instead of
// SceneRender's live TNKN=9 chunk window — the whole reason this was
// coupled to chunks_ in the first place was that the chunk array was the
// only in-memory height source available; TerrainAtlas has been that same
// full-world source since long before this rewrite, just never used here.
// Bonus: md.terrain_height/probe-height now work ANYWHERE in the world,
// not only inside whichever 9x9 window happens to be streamed in.
// IsWalkable/TerrainPassGrid/TerrainAtlas_IsWalkableWorld REMOVED entirely
// (physical-design refactor Phase -1, 2026-09-17) — zero external callers
// anywhere, confirmed by grep before deletion. See the removal note where
// IsWalkable() used to be, below.

#include <monkey_dust/world/terrain_chunk.h>
#include <monkey_dust/world/chunk_def.h>
#include <monkey_dust/world/terrain_gen.h>
#include <cmath>

class TerrainQuery {
public:
    static TerrainQuery& Get() {
        static TerrainQuery s;
        return s;
    }

    // Call once after terrain chunks are loaded, and again any time the
    // streaming window shifts (world_off_x/z or zone_ox/z change) — cheap,
    // no chunk data copied, just remembers the mapping between the
    // caller's session-local floating coordinate space (world_off_x/z,
    // e.g. SceneRender::tnoff_x/z — see that field's own doc comment for
    // why it's not the same as absolute Kenshi metres) and TerrainAtlas's
    // absolute-metres space (zone_ox/z, in zone units 0..63, e.g.
    // SceneRender::zone_ox/z).
    //
    // chunks/tnkn/cx_base/cz_base: still needed for GetHeight's own
    // chunk-sampled fallback (below) — pass zone_ox/zone_oz = -1 (default)
    // to disable the atlas-based height path and fall back to the old
    // chunk-sampled one, e.g. for a caller with no real Kenshi zone
    // mapping. (IsWalkable(), the other former reader, was removed —
    // physical-design refactor Phase -1, 2026-09-17 — zero external
    // callers.)
    void Init(TerrainChunk* chunks,  // [tnkn][tnkn] row-major
              int    tnkn,           // chunks per side
              float  chunk_size,
              float  world_off_x,
              float  world_off_z,
              int    cx_base = 0,
              int    cz_base = 0,
              int    zone_ox = -1,
              int    zone_oz = -1) {
        chunks_      = chunks;
        tnkn_        = tnkn;
        chunk_size_  = chunk_size;
        world_off_x_ = world_off_x;
        world_off_z_ = world_off_z;
        cx_base_     = cx_base;
        cz_base_     = cz_base;
        zone_ox_     = zone_ox;
        zone_oz_     = zone_oz;
        has_atlas_mapping_ = (zone_ox >= 0 && zone_oz >= 0);
        ready_       = true;
    }

    bool IsReady() const { return ready_; }

    // Ground height at world (wx, wz). Returns false only if TerrainQuery
    // was never Init()ed or TerrainAtlas isn't loaded — TerrainAtlas_
    // SampleWorld itself clamps to the nearest valid edge rather than
    // failing, so this now always succeeds anywhere in the real Kenshi
    // world once ready (falls back to the old chunk-window-bounded
    // behaviour when Init() didn't get a real zone_ox/zone_oz).
    bool GetHeight(float wx, float wz, float& out_h) const {
        if (!ready_) return false;
        if (has_atlas_mapping_ && TerrainAtlas_Loaded()) {
            float ax = wx - world_off_x_ + (float)zone_ox_ * chunk_size_;
            float az = wz - world_off_z_ + (float)zone_oz_ * chunk_size_;
            out_h = TerrainAtlas_SampleWorld(ax, az);
            return true;
        }
        float gx = wx - world_off_x_;
        float gz = wz - world_off_z_;
        int   cx = (int)(gx / chunk_size_);
        int   cz = (int)(gz / chunk_size_);
        if (cx < 0 || cx >= tnkn_ || cz < 0 || cz >= tnkn_) return false;
        int pcx = (cx + cx_base_) % tnkn_;
        int pcz = (cz + cz_base_) % tnkn_;
        const TerrainChunk& chunk = chunks_[pcz * tnkn_ + pcx];
        if (!chunk.loaded) return false;
        float lx = gx - cx * chunk_size_;
        float lz = gz - cz * chunk_size_;
        out_h = chunk.SampleHeight(lx, lz);
        return true;
    }

    // Convenience overload — returns 0 when outside terrain (safe for non-entity use).
    float GetHeight(float wx, float wz) const {
        float h = 0.f;
        GetHeight(wx, wz, h);
        return h;
    }

    // Surface normal at world (wx, wz) — central difference over 0.5m.
    // Returns (0,1,0) if outside loaded terrain.
    void GetNormal(float wx, float wz,
                   float& nx, float& ny, float& nz) const {
        static constexpr float D = 0.5f;
        float hL = GetHeight(wx - D, wz);
        float hR = GetHeight(wx + D, wz);
        float hB = GetHeight(wx, wz - D);
        float hF = GetHeight(wx, wz + D);
        // N = tangentZ × tangentX  (upward-facing normal)
        float tx = 2.f * D, ty = hR - hL, tz = 0.f;
        float bx = 0.f,     by = hF - hB, bz = 2.f * D;
        nx = by * tz - bz * ty;
        ny = bz * tx - bx * tz;
        nz = bx * ty - by * tx;
        float len = sqrtf(nx*nx + ny*ny + nz*nz);
        if (len > 1e-6f) { nx /= len; ny /= len; nz /= len; }
        else             { nx = 0.f; ny = 1.f; nz = 0.f; }
    }

    // IsWalkable(wx, wz) REMOVED (physical-design refactor Phase -1,
    // 2026-09-17): zero external callers found anywhere in engine/game/
    // tools (grep-confirmed before deletion) — the L2-style passability
    // subsystem it depended on (TerrainAtlas_IsWalkableWorld, TerrainChunk
    // ::pass_grid/TerrainPassGrid) was never actually wired into any real
    // AI/pathfinding call site; NavSystem's Detour navmesh is the real
    // walkability source. Removed together in the same commit, not left
    // as orphaned dead code behind this one.

    // G-3: water level constant and query.
    // Returns true if terrain height at (wx,wz) is below the water plane.
    // NavMesh excludes water cells; water.frag renders the flat plane.
    static constexpr float WATER_LEVEL = -2.0f;  // metres — terrain below this = underwater
    bool IsWater(float wx, float wz) const {
        return GetHeight(wx, wz) < WATER_LEVEL;
    }

    // Slope angle in radians (0 = flat, π/2 = vertical cliff).
    float GetSlope(float wx, float wz) const {
        float nx, ny, nz;
        GetNormal(wx, wz, nx, ny, nz);
        return acosf(ny < 1.f ? ny : 1.f);
    }

private:
    TerrainQuery() = default;
    TerrainChunk* chunks_      = nullptr;
    int           tnkn_        = 0;
    float         chunk_size_  = 64.f;
    float         world_off_x_ = 0.f;
    float         world_off_z_ = 0.f;
    int           cx_base_     = 0;
    int           cz_base_     = 0;
    int           zone_ox_     = -1;
    int           zone_oz_     = -1;
    bool          has_atlas_mapping_ = false;
    bool          ready_       = false;
};

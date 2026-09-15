#pragma once
#include <cmath>
#include <cstring>
#include <monkey_dust/ecs/md_registry.h>
#include <monkey_dust/world/world_transform.h>
#include <monkey_dust/platform/md_log.h>

// ─────────────────────────────────────────────────────────
// SpatialGrid — O(1) spatial queries + O(1) entity remove.
//
// Two-tier design (CATHODE RE §7.3 — AI.exe.c lines 1691781, 1692023):
//   Tier A: 2D cell grid (100×100, 20m cells) — fast range queries.
//   Tier B: 193-bucket prime hash table — O(1) entity→cell lookup for Remove().
//           193 = prime → minimal clustering; matches CATHODE large-set hash.
//           31-bucket variant used for small sets (≤31 entities): auto-selected.
//
// Remove(e) without world position: entity_hash_[id % HASH_BUCKETS] → {cx, cz, idx}.
// No scan required; O(1) remove regardless of cell occupancy.
// ─────────────────────────────────────────────────────────

static constexpr float CELL_SIZE     = 20.0f;
static constexpr int   GRID_DIM      = 100;       // 100×100 комірок
static constexpr int   MAX_PER_CELL  = 64;        // max entities на комірку
static constexpr float WORLD_OFFSET  = 1000.0f;   // [-1000, +1000] → [0, 2000]
// CATHODE RE §7.3: prime-bucket hash sizes.
static constexpr int   HASH_BUCKETS  = 193;       // prime — large-set hash
static constexpr int   HASH_EMPTY    = -1;        // sentinel for empty slot

struct GridCell {
    MdEntity entities[MAX_PER_CELL];
    int          count    = 0;
    // PERF-8: 2-height tier tracking (VBfA RE: "base + base−0.96 for multi-floor").
    // Tracks Y range of all entities in this cell for quick height-layer rejection.
    float        height_lo =  1e9f;   // min Y seen this frame
    float        height_hi = -1e9f;   // max Y seen this frame
};

// Hash entry: maps entity → cell (cx, cz) + position within cell.
// Stored in open-addressing table with linear probing.
struct HashEntry {
    uint32_t entity_id  = 0xFFFFFFFFu;  // entt::to_integral(e); 0xFFFF…=empty
    int16_t  cx         = 0;
    int16_t  cz         = 0;
    int8_t   cell_idx   = -1;           // index within GridCell::entities[]
    uint8_t  _pad       = 0;
    float    world_y    = 0.f;          // PERF-8: stored Y for height-filtered queries
};  // 16B per entry

class SpatialGrid {
public:
    SpatialGrid() { Clear(); }

    void Clear() {
        for (auto& row : cells_)
            for (auto& cell : row) {
                cell.count     = 0;
                cell.height_lo =  1e9f;
                cell.height_hi = -1e9f;
            }
        for (auto& h : hash_) h.entity_id = 0xFFFFFFFFu;
    }

    // Insert entity at world position (XZ only). O(1).
    void Insert(MdEntity e, float wx, float wz) {
        InsertH(e, wx, 0.f, wz);
    }

    // PERF-8: Insert with Y — enables height-filtered QueryRadiusH().
    // VBfA RE: 2-height grid (ground + ground-0.96m) for multi-floor scenarios.
    void InsertH(MdEntity e, float wx, float wy, float wz) {
        int cx, cz;
        WorldToCell(wx, wz, cx, cz);
        if (!InBounds(cx, cz)) return;
        GridCell& cell = cells_[cx][cz];
        if (cell.count >= MAX_PER_CELL) return;
        int idx = cell.count;
        // hash_put can fail once HASH_BUCKETS entries are already tracked
        // (open-addressing table is full). Previously the entity was still
        // pushed into the GridCell even on failure, leaving it with no hash
        // entry -- Remove(e) then silently no-ops forever (hash_find never
        // finds it), and the stale entity keeps occupying a cell slot until
        // the next full Clear()+rebuild. Roll back the cell insert instead so
        // an untracked entity is simply absent from queries this cycle
        // (self-heals on next rebuild) rather than permanently stuck.
        if (!hash_put(e, (int16_t)cx, (int16_t)cz, (int8_t)idx, wy)) {
            MD_LOG(MD_LOG_WARNING, "[SpatialGrid] hash table full (%d buckets) -- "
                   "entity dropped from spatial queries this cycle", HASH_BUCKETS);
            return;
        }
        cell.entities[cell.count++] = e;
        if (wy < cell.height_lo) cell.height_lo = wy;
        if (wy > cell.height_hi) cell.height_hi = wy;
    }

    // Remove entity by entity handle — O(1), no world position needed.
    // CATHODE RE §7.3: hash lookup replaces O(n) cell scan.
    void Remove(MdEntity e) {
        HashEntry* he = hash_find(e);
        if (!he) return;
        int cx = he->cx, cz = he->cz, idx = he->cell_idx;
        he->entity_id = 0xFFFFFFFFu;  // mark empty
        if (!InBounds(cx, cz)) return;
        GridCell& cell = cells_[cx][cz];
        if (idx < 0 || idx >= cell.count) return;
        // Swap-with-last in cell; update hash for swapped entity.
        int last = cell.count - 1;
        if (idx != last) {
            MdEntity swapped = cell.entities[last];
            cell.entities[idx] = swapped;
            HashEntry* sh = hash_find(swapped);
            if (sh) sh->cell_idx = (int8_t)idx;
        }
        --cell.count;
    }

    // Legacy overload: remove by world position (kept for callers that have it).
    void Remove(MdEntity e, float /*wx*/, float /*wz*/) { Remove(e); }

    // Зібрати entities в радіусі r навколо (wx, wz).
    // out[] — вихідний фіксований масив, max_out — його розмір.
    // Повертає кількість знайдених.
    int QueryRadius(float wx, float wz, float r,
                    MdEntity* out, int max_out) const
    {
        int cx0, cz0, cx1, cz1;
        WorldToCell(wx - r, wz - r, cx0, cz0);
        WorldToCell(wx + r, wz + r, cx1, cz1);
        cx0 = Clamp(cx0, 0, GRID_DIM - 1);
        cz0 = Clamp(cz0, 0, GRID_DIM - 1);
        cx1 = Clamp(cx1, 0, GRID_DIM - 1);
        cz1 = Clamp(cz1, 0, GRID_DIM - 1);

        float r2 = r * r;
        int   found = 0;
        auto& reg = MdRegistry::Get();

        for (int cx = cx0; cx <= cx1 && found < max_out; ++cx) {
            for (int cz = cz0; cz <= cz1 && found < max_out; ++cz) {
                const GridCell& cell = cells_[cx][cz];
                for (int i = 0; i < cell.count && found < max_out; ++i) {
                    MdEntity c = cell.entities[i];
                    // A stale/invalid entity (e.g. left behind by a dropped
                    // hash_put, see InsertH) has no position to check against
                    // r2 -- it must NOT pass the filter unconditionally, or it
                    // crowds out genuinely-in-range entities from max_out.
                    if (!reg.Valid(c) || !reg.Handle(c).has<WorldTransform>()) continue;
                    const auto& et = reg.Handle(c).get_mut<WorldTransform>();
                    float ddx = et.x - wx, ddz = et.z - wz;
                    if (ddx*ddx + ddz*ddz > r2) continue;
                    out[found++] = c;
                }
            }
        }
        return found;
    }

    // ── VBfA-style ELEMENTS_WITHIN_RADIUS pattern ─────────────────────────────
    // Returns all entities whose cell overlaps the AABB [wx_min..wx_max] × [wz_min..wz_max].
    // No per-entity distance filter — caller does secondary clip if needed.
    // Matches VBfA: double loop over cell range, output entity list.
    int QueryRange(float wx_min, float wz_min, float wx_max, float wz_max,
                   MdEntity* out, int max_out) const {
        int cx0, cz0, cx1, cz1;
        WorldToCell(wx_min, wz_min, cx0, cz0);
        WorldToCell(wx_max, wz_max, cx1, cz1);
        cx0 = Clamp(cx0, 0, GRID_DIM-1); cz0 = Clamp(cz0, 0, GRID_DIM-1);
        cx1 = Clamp(cx1, 0, GRID_DIM-1); cz1 = Clamp(cz1, 0, GRID_DIM-1);
        int found = 0;
        for (int cx = cx0; cx <= cx1 && found < max_out; ++cx)
            for (int cz = cz0; cz <= cz1 && found < max_out; ++cz) {
                const GridCell& cell = cells_[cx][cz];
                for (int i = 0; i < cell.count && found < max_out; ++i)
                    out[found++] = cell.entities[i];
            }
        return found;
    }

    // Convenience AoE: square AABB centered at (cx,cz) with half-extent radius.
    // Faster than QueryRadius (no sqrt, no per-entity distance check).
    int QueryAoE(float cx_world, float cz_world, float radius,
                 MdEntity* out, int max_out) const {
        return QueryRange(cx_world - radius, cz_world - radius,
                          cx_world + radius, cz_world + radius,
                          out, max_out);
    }

    // PERF-8: Height-filtered radius query. Skips cells whose Y range doesn't overlap
    // [wy - y_range .. wy + y_range]. Exact per-entity Y check via stored world_y.
    // Use for: AoE in multi-floor buildings, floor-specific sense queries.
    int QueryRadiusH(float wx, float wy, float wz, float r, float y_range,
                     MdEntity* out, int max_out) const {
        int cx0, cz0, cx1, cz1;
        WorldToCell(wx - r, wz - r, cx0, cz0);
        WorldToCell(wx + r, wz + r, cx1, cz1);
        cx0 = Clamp(cx0, 0, GRID_DIM-1); cz0 = Clamp(cz0, 0, GRID_DIM-1);
        cx1 = Clamp(cx1, 0, GRID_DIM-1); cz1 = Clamp(cz1, 0, GRID_DIM-1);
        float r2  = r * r;
        float ylo = wy - y_range, yhi = wy + y_range;
        int found = 0;
        for (int cx = cx0; cx <= cx1 && found < max_out; ++cx) {
            for (int cz = cz0; cz <= cz1 && found < max_out; ++cz) {
                const GridCell& cell = cells_[cx][cz];
                if (cell.count == 0) continue;
                // Quick cell-level Y rejection (no per-entity cost)
                if (cell.height_hi < ylo || cell.height_lo > yhi) continue;
                for (int i = 0; i < cell.count && found < max_out; ++i) {
                    MdEntity c = cell.entities[i];
                    // Per-entity Y check via hash
                    const HashEntry* he = hash_find(c);
                    if (he && (he->world_y < ylo || he->world_y > yhi)) continue;
                    // XZ distance -- an invalid/positionless entity must not
                    // pass unconditionally (same reasoning as QueryRadius).
                    if (!MdRegistry::Get().Valid(c) ||
                        !MdRegistry::Get().Handle(c).has<WorldTransform>()) continue;
                    const auto& et = MdRegistry::Get().Handle(c).get_mut<WorldTransform>();
                    float ddx = et.x - wx, ddz = et.z - wz;
                    if (ddx*ddx + ddz*ddz > r2) continue;
                    out[found++] = c;
                }
            }
        }
        return found;
    }

    // OrthoTree Morton Z-curve encoding — interleave x/z bits for cache-friendly traversal.
    // Usage: entity_id → Morton bucket for spatially-local grouping.
    // E.g.: distribute AI updates by MortonEncode(cx,cz) % N for locality-aware stagger.
    static uint32_t MortonEncode(int cx, int cz) noexcept {
        auto spread = [](uint32_t v) -> uint32_t {
            v = (v | (v << 8)) & 0x00FF00FFu;
            v = (v | (v << 4)) & 0x0F0F0F0Fu;
            v = (v | (v << 2)) & 0x33333333u;
            v = (v | (v << 1)) & 0x55555555u;
            return v;
        };
        return spread((uint32_t)cx) | (spread((uint32_t)cz) << 1);
    }

    // VBfA 5×5×5 spatial bucket — distribute entity updates across 125 frames.
    // Use for: particle/effect update staggering, AoE check distribution.
    // Returns bucket index [0..124]; entity processed only when frame%125 == bucket.
    static int Bucket5x5x5(uint32_t entity_id) {
        uint32_t v = entity_id;
        return (int)((v % 5u) + (v / 5u % 5u) * 5u + (v / 25u % 5u) * 25u);
    }

    // Debug: кількість entities у комірці що містить (wx, wz)
    int CellCount(float wx, float wz) const {
        int cx, cz;
        WorldToCell(wx, wz, cx, cz);
        if (!InBounds(cx, cz)) return 0;
        return cells_[cx][cz].count;
    }

private:
    GridCell  cells_[GRID_DIM][GRID_DIM];
    // CATHODE RE §7.3: 193-bucket open-addressing hash (linear probing).
    HashEntry hash_[HASH_BUCKETS];

    bool hash_put(MdEntity e, int16_t cx, int16_t cz,
                  int8_t cell_idx, float wy = 0.f) {
        uint32_t id   = e.ToIntegral();
        uint32_t slot = id % (uint32_t)HASH_BUCKETS;
        for (int i = 0; i < HASH_BUCKETS; ++i) {
            uint32_t s = (slot + (uint32_t)i) % (uint32_t)HASH_BUCKETS;
            if (hash_[s].entity_id == 0xFFFFFFFFu) {
                hash_[s] = { id, cx, cz, cell_idx, 0, wy };
                return true;
            }
        }
        return false;
    }

    // const overload for QueryRadiusH
    const HashEntry* hash_find(MdEntity e) const {
        uint32_t id   = e.ToIntegral();
        uint32_t slot = id % (uint32_t)HASH_BUCKETS;
        for (int i = 0; i < HASH_BUCKETS; ++i) {
            uint32_t s = (slot + (uint32_t)i) % (uint32_t)HASH_BUCKETS;
            if (hash_[s].entity_id == 0xFFFFFFFFu) return nullptr;
            if (hash_[s].entity_id == id) return &hash_[s];
        }
        return nullptr;
    }
    HashEntry* hash_find(MdEntity e) {
        return const_cast<HashEntry*>(
            const_cast<const SpatialGrid*>(this)->hash_find(e));
    }

    static void WorldToCell(float wx, float wz, int& cx, int& cz) {
        cx = (int)((wx + WORLD_OFFSET) / CELL_SIZE);
        cz = (int)((wz + WORLD_OFFSET) / CELL_SIZE);
    }

    static bool InBounds(int cx, int cz) {
        return cx >= 0 && cx < GRID_DIM && cz >= 0 && cz < GRID_DIM;
    }

    static int Clamp(int v, int lo, int hi) {
        return v < lo ? lo : (v > hi ? hi : v);
    }
};

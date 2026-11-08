#pragma once
// Per-category frame counters shown in F3 -> Debug (G3 cheap half, PERF_SEARCH_2026-11-08 item 3, Jupiter EX study item 3).
// The systems already count their own draws / instances / triangles; this only sums them by category. No per-frame work of its own.
#include <cstdint>

namespace md {

struct FrameCounterRow {
    const char* name = "";
    int      draws     = 0;
    int      instances = 0;
    int64_t  triangles = 0;
};

constexpr int kFrameCounterMaxRows = 8;

struct FrameCounters {
    FrameCounterRow rows[kFrameCounterMaxRows];
    int count = 0;

    // false when the table is full
    bool Add(const char* name, int draws, int instances, int64_t triangles) {
        if (count >= kFrameCounterMaxRows) return false;
        rows[count++] = FrameCounterRow{name, draws, instances, triangles};
        return true;
    }
    FrameCounterRow Total() const {
        FrameCounterRow t; t.name = "total";
        for (int i = 0; i < count; ++i) { t.draws += rows[i].draws; t.instances += rows[i].instances; t.triangles += rows[i].triangles; }
        return t;
    }
    // triangles per instance, 0 when there are none (a small value at distance = the LOD candidates)
    static float TrisPerInstance(const FrameCounterRow& r) { return r.instances > 0 ? (float)((double)r.triangles / (double)r.instances) : 0.f; }
};

}  // namespace md

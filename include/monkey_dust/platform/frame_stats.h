#pragma once
#include <chrono>
#include <cstdio>
#include <cstring>
#ifdef __linux__
#include <unistd.h>
#ifdef MD_TRACY
#include <tracy/TracyC.h>   // profiling preset only (MD_TRACY=ON); the release binary has none of it
#endif
#include <time.h>
#endif

// FrameStats — lightweight Release-safe frame time breakdown.
// Usage:
//   FrameStats::Get().Begin("NpcRender");
//   ... work ...
//   FrameStats::Get().End("NpcRender");
//   FrameStats::Get().EndFrame(dt);  // once per frame

#ifdef __linux__
// render-audit-2026 (docs/RENDER_AUDIT_2026.md §11.2): distinguishes
// "thread was off-CPU" (scheduler preemption or a blocking syscall) from
// "thread was on-CPU doing uninstrumented work" for the RenderTotal span,
// per reviewer's request -- both proc files are cheap (no allocation,
// small fixed reads), fine to sample twice per frame for a diagnostic.
struct MdOsStatSnapshot {
    long long sched_wait_ns;  // /proc/self/schedstat field[1]: total time spent
                               // waiting on a runqueue (off-CPU, runnable)
    long long thread_cpu_ns;  // CLOCK_THREAD_CPUTIME_ID (on-CPU time, THIS
                               // thread only, ns resolution)
    long long majflt;         // /proc/self/stat field 12 (major page faults)
};

inline MdOsStatSnapshot MdReadOsStats() {
    MdOsStatSnapshot s{0, 0, 0};
    if (FILE* f = fopen("/proc/self/schedstat", "r")) {
        long long run_time_ns = 0, timeslices = 0;
        if (fscanf(f, "%lld %lld %lld", &run_time_ns, &s.sched_wait_ns, &timeslices) != 3)
            s.sched_wait_ns = 0;
        fclose(f);
    }
    // render-audit-2026 §11.3 follow-up: utime+stime from /proc/self/stat
    // was WRONG for this purpose on two counts -- (1) USER_HZ quantization
    // (typically 100Hz = 10ms/tick, so a real 19-27ms span can sample as a
    // 0-tick delta depending on when the timer interrupt lands), and (2)
    // /proc/self/stat's utime/stime may reflect the whole thread GROUP, not
    // just the calling thread, when read via the "self" alias. Both false
    // negatives masked genuine on-CPU work (JoltWorld::AddTerrainMesh) as
    // if the thread had been sleeping. CLOCK_THREAD_CPUTIME_ID is ns-
    // resolution and unambiguously scoped to the calling thread.
    struct timespec ts;
    if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts) == 0)
        s.thread_cpu_ns = (long long)ts.tv_sec * 1000000000LL + ts.tv_nsec;
    if (FILE* f = fopen("/proc/self/stat", "r")) {
        char buf[512];
        size_t n = fread(buf, 1, sizeof(buf) - 1, f);
        fclose(f);
        buf[n] = '\0';
        // comm (field 2) is "(name)" and may itself contain spaces/parens --
        // skip to the LAST ')' before tokenizing the fixed-format remainder.
        char* rparen = strrchr(buf, ')');
        if (rparen) {
            // Fields after comm, 1-indexed from here: state(1) ppid(2) ...
            // majflt is field 12 overall = field 10 after comm (man proc(5)).
            char* p = rparen + 1;
            long long vals[16] = {0};
            int i = 0;
            while (*p && i < 16) {
                while (*p == ' ') ++p;
                if (!*p) break;
                vals[i++] = strtoll(p, &p, 10);
            }
            if (i >= 10) s.majflt = vals[9];  // field 12 overall
        }
    }
    return s;
}
#endif  // __linux__

static constexpr int FS_MAX_SLOTS = 32;
static constexpr float FS_REPORT_INTERVAL = 5.f;  // seconds
// render-audit-2026 (docs/RENDER_AUDIT_2026.md §11.1/§2.2): the 5s
// avg/max report aggregates ACROSS frames, so a "RenderTotal max=42ms"
// and a "DrawScene max=20ms" in the same report line are not guaranteed
// to be the same frame -- no section can be blamed or cleared from that
// alone. FS_SPIKE_THRESHOLD_MS gates a per-frame raw dump (all slots'
// single most recent value, not the aggregate) whenever RenderTotal
// itself exceeds this in one frame, which is frame-exact by construction.
static constexpr float FS_SPIKE_THRESHOLD_MS = 25.f;

struct FrameSlot {
    char  name[24];
    float sum_ms;
    float max_ms;
    float last_ms;  // this frame's raw value (not averaged) -- spike dump
    int   frames;
    double begin_s;
#ifdef MD_TRACY
    TracyCZoneCtx tracy_ctx;
#endif
};

class FrameStats {
public:
    static FrameStats& Get() { static FrameStats inst; return inst; }

    void Begin(const char* name) {
        int i = FindOrCreate(name);
        if (i >= 0) slots_[i].begin_s = Now();
#ifdef MD_TRACY
        if (i >= 0) {
            uint64_t loc = ___tracy_alloc_srcloc_name(0, __FILE__, sizeof(__FILE__) - 1, "FrameStats", 10, name, strlen(name), 0);
            slots_[i].tracy_ctx = ___tracy_emit_zone_begin_alloc(loc, 1);
        }
#endif
        // render-audit-2026: RenderTotal's Begin() is the one call site that
        // fires exactly once per frame, before any other section -- use it
        // to zero every slot's last_ms so End() below can safely ACCUMULATE
        // rather than overwrite. Without this, a section entered multiple
        // times in one frame (e.g. TerrainStreamQueue::poll()'s per-slot
        // loop, which can process several ready chunks in a single call)
        // only ever showed its LAST iteration's cost in [FRAMESPIKE], hiding
        // the true per-frame total across all iterations.
        if (strncmp(name, "RenderTotal", 23) == 0) {
            for (int k = 0; k < count_; ++k) slots_[k].last_ms = 0.f;
#ifdef __linux__
            // RenderTotal spans the whole frame (main.cpp) -- snapshot here,
            // diffed in End() below, so CheckFrameSpike() can tell "off-CPU"
            // from "on-CPU but uninstrumented" for the exact spiking frame.
            os_begin_ = MdReadOsStats();
#endif
        }
    }

    void End(const char* name) {
        int i = Find(name);
        if (i < 0) return;
#ifdef MD_TRACY
        ___tracy_emit_zone_end(slots_[i].tracy_ctx);
        if (strncmp(name, "RenderTotal", 23) == 0) ___tracy_emit_frame_mark(nullptr);
#endif
        float ms = (float)((Now() - slots_[i].begin_s) * 1000.0);
        slots_[i].sum_ms += ms;
        if (ms > slots_[i].max_ms) slots_[i].max_ms = ms;
        slots_[i].last_ms += ms;  // accumulate -- see Begin("RenderTotal")'s reset above
        slots_[i].frames++;
#ifdef __linux__
        if (strncmp(name, "RenderTotal", 23) == 0) {
            MdOsStatSnapshot e = MdReadOsStats();
            os_sched_wait_ms_ = (float)(e.sched_wait_ns - os_begin_.sched_wait_ns) / 1.0e6f;
            os_cpu_ms_ = (float)(e.thread_cpu_ns - os_begin_.thread_cpu_ns) / 1.0e6f;
            os_majflt_ = e.majflt - os_begin_.majflt;
        }
#endif
    }

    // render-audit-2026: fires once per frame, independent of the 5s
    // aggregate report below, whenever THIS frame's RenderTotal exceeds
    // FS_SPIKE_THRESHOLD_MS. Dumps every slot's raw last_ms (frame-exact,
    // not avg/max across a window) plus the sum of all non-RenderTotal
    // slots vs RenderTotal itself -- a large (sum < RenderTotal) gap means
    // time is passing in code with NO FS_BEGIN/END wrapper at all (the
    // real answer to "which section is it" may be "none of them").
    void CheckFrameSpike() const {
        int rt = Find("RenderTotal");
        if (rt < 0 || slots_[rt].last_ms < FS_SPIKE_THRESHOLD_MS) return;
        // render-audit-2026: bumped 512->1536 once section count grew past
        // ~20 -- the old size silently truncated the line right before the
        // sched_wait/cpu/majflt suffix (snprintf still returns success, no
        // compiler/runtime warning), which looked like a missing feature
        // rather than a buffer-size bug. Fixed-size buffer per project rule.
        char buf[1536];
        char* p = buf;
        p += snprintf(p, 256, "[FRAMESPIKE] RenderTotal=%.1fms | ", slots_[rt].last_ms);
        float sum_others = 0.f;
        for (int i = 0; i < count_; ++i) {
            if (i == rt) continue;
            sum_others += slots_[i].last_ms;
            p += snprintf(p, (size_t)(buf + sizeof(buf) - p),
                          "%s=%.1f ", slots_[i].name, slots_[i].last_ms);
        }
        p += snprintf(p, (size_t)(buf + sizeof(buf) - p),
                 "| sum_others=%.1f unattributed=%.1f",
                 sum_others, slots_[rt].last_ms - sum_others);
#ifdef __linux__
        // render-audit-2026 (§11.2 follow-up): distinguishes off-CPU
        // (sched_wait) from on-CPU-but-uninstrumented (cpu_ms) for
        // whatever unattributed gap this frame shows above.
        snprintf(p, (size_t)(buf + sizeof(buf) - p),
                 " | sched_wait=%.1fms cpu=%.1fms majflt=%lld",
                 os_sched_wait_ms_, os_cpu_ms_, os_majflt_);
#endif
        fprintf(stderr, "%s\n", buf);
    }

    // Call once per frame with frame dt (seconds).
    // Returns formatted string when report fires (every 5s), else nullptr.
    // terrain_pending/vt_resident (task #635, docs/GRANITE_P5_BASELINE.md
    // reproducibility concern): P5-style perf baselines assumed "same
    // camera pose == same scene" without checking whether terrain
    // streaming/VT-cache had actually settled by the time of measurement
    // (an 8s settle timer is a wait, not a verified precondition).
    //
    // First cut (2026-09-02) reported the INSTANTANEOUS value at the one
    // EndFrame() call where the 5s report happens to fire -- a live check
    // at zone(24,12) with active streaming showed TerrainPending=0 in the
    // very same line as non-zero TerrainStreamPoll/ClutterUpload costs,
    // because the queue drained between report ticks. That made the
    // counter unable to distinguish "never pending this window" from
    // "was pending, but not at the exact instant we sampled." Now tracks
    // a running max + count of frames with pending>0 across the WHOLE
    // window, so "was terrain fully settled for the entire 5s" is a
    // checkable fact instead of a coin flip on report timing. Pass 0/0
    // from call sites where terrain streaming/VT cache don't apply (e.g.
    // editor, tests).
    const char* EndFrame(float dt, int fps, int npc_count,
                         int terrain_pending = 0, int vt_resident = 0) {
        CheckFrameSpike();
        accum_ += dt;
        frame_count_++;
        if (terrain_pending > terrain_pending_max_) terrain_pending_max_ = terrain_pending;
        if (terrain_pending > 0) ++terrain_pending_nonzero_frames_;
        if (vt_resident > vt_resident_max_) vt_resident_max_ = vt_resident;
        if (accum_ < FS_REPORT_INTERVAL) return nullptr;

        char* p = report_buf_;
        p += snprintf(p, 256,
                      "[PERF] %d FPS | NPCs=%d | "
                      "TerrainPendingMax=%d(%d/%dfr>0) VtResidentMax=%d | ",
                      fps, npc_count, terrain_pending_max_,
                      terrain_pending_nonzero_frames_, frame_count_, vt_resident_max_);
        for (int i = 0; i < count_; ++i) {
            float avg = slots_[i].frames > 0
                      ? slots_[i].sum_ms / (float)slots_[i].frames : 0.f;
            p += snprintf(p, (size_t)(report_buf_ + sizeof(report_buf_) - p),
                          "%s=%.1fms(max%.1f) ", slots_[i].name, avg, slots_[i].max_ms);
            slots_[i].sum_ms = 0.f;
            slots_[i].max_ms = 0.f;
            slots_[i].frames = 0;
        }
        accum_ = 0.f;
        frame_count_ = 0;
        terrain_pending_max_ = 0;
        terrain_pending_nonzero_frames_ = 0;
        vt_resident_max_ = 0;
        fprintf(stderr, "%s\n", report_buf_);
        return report_buf_;
    }

    int Count() const { return count_; }
    const FrameSlot& GetAt(int i) const { return slots_[i]; }

private:
    FrameStats() : count_(0), accum_(0.f), frame_count_(0) {
        memset(slots_, 0, sizeof(slots_));
        memset(report_buf_, 0, sizeof(report_buf_));
    }

    FrameSlot slots_[FS_MAX_SLOTS];
    int       count_;
    float     accum_;
    int       frame_count_;
    char      report_buf_[1536];
    // task #635 follow-up: windowed aggregate for terrain_pending/
    // vt_resident, see EndFrame()'s doc comment for why instantaneous
    // wasn't enough.
    int       terrain_pending_max_ = 0;
    int       terrain_pending_nonzero_frames_ = 0;
    int       vt_resident_max_ = 0;
#ifdef __linux__
    MdOsStatSnapshot os_begin_{0, 0, 0};
    float os_sched_wait_ms_ = 0.f;
    float os_cpu_ms_ = 0.f;
    long long os_majflt_ = 0;
#endif

    static double Now() {
        return std::chrono::duration<double>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    int Find(const char* name) const {
        for (int i = 0; i < count_; ++i)
            if (strncmp(slots_[i].name, name, 23) == 0) return i;
        return -1;
    }
    int FindOrCreate(const char* name) {
        int i = Find(name);
        if (i >= 0) return i;
        if (count_ >= FS_MAX_SLOTS) return -1;
        i = count_++;
        memset(&slots_[i], 0, sizeof(slots_[i]));
        strncpy(slots_[i].name, name, 23);
        return i;
    }
};

#define FS_BEGIN(name) FrameStats::Get().Begin(name)
#define FS_END(name)   FrameStats::Get().End(name)

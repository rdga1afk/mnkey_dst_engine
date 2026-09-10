#include <monkey_dust/platform/job_graph.h>
#include <monkey_dust/platform/job_system.h>
#include <monkey_dust/platform/md_log.h>
#include <monkey_dust/ecs/md_registry.h>
#include <cstring>

namespace md {

#if !defined(MD_ECS_GAIA)
namespace {
// Wraps one batch's (fn, user) with the flecs stage it must run through —
// JobSystem::Submit only carries a single void* payload, so the stage
// index rides along in this struct instead. StagedThunk sets up the
// thread-local MdRegistryStageScope for the DURATION of the real fn()
// call, on whichever worker thread picks this job up, then tears it down
// — see md_registry.h's task #7/#8 concurrency section for why this
// specific pattern (readonly_begin(true) + per-batch stage + queries
// built once against the main world) is the only one confirmed safe by
// probe, out of several that looked equivalent on paper.
struct StagedJob {
    JobBatchFn fn;
    void* user;
    flecs::world stage;
};
StagedJob g_staged_jobs[JobGraph::MAX_BATCHES];

void StagedThunk(void* p) {
    auto* job = static_cast<StagedJob*>(p);
    MdRegistryStageScope scope(job->stage);
    job->fn(job->user);
}
} // namespace
#endif

JobGraph& JobGraph::Get() {
    static JobGraph inst;
    return inst;
}

bool JobGraph::AddBatch(const char* name, const JobBatchDesc& desc, JobBatchFn fn, void* user) {
    if (!name || !name[0] || !fn) return false;
    if (count_ >= MAX_BATCHES) {
        MD_LOG(MD_LOG_WARNING, "[JobGraph] MAX_BATCHES=%d reached, cannot register '%s'",
               MAX_BATCHES, name);
        return false;
    }
    Entry& e = entries_[count_++];
    strncpy(e.name, name, sizeof(e.name) - 1);
    e.desc = desc;
    e.fn   = fn;
    e.user = user;
    return true;
}

bool JobGraph::Conflicts(const JobBatchDesc& a, const JobBatchDesc& b) {
    auto overlaps = [](const uint32_t* xs, int xn, const uint32_t* ys, int yn) {
        for (int i = 0; i < xn; ++i)
            for (int j = 0; j < yn; ++j)
                if (xs[i] == ys[j]) return true;
        return false;
    };
    // a writes vs b reads/writes
    if (overlaps(a.writes, a.write_count, b.reads,  b.read_count))  return true;
    if (overlaps(a.writes, a.write_count, b.writes, b.write_count)) return true;
    // b writes vs a reads
    if (overlaps(b.writes, b.write_count, a.reads,  a.read_count))  return true;
    return false;
}

void JobGraph::Run() {
    // Validate: for each batch P reading/writing resource R, the last
    // batch that WRITES R must be registered before P (same check shape
    // as RenderPassGraph::Validate() — catches wrong registration order,
    // does not by itself make anything run in parallel, see header note).
    //
    // Point 3 (concurrency audit): escalated WARNING -> ERROR. Deliberately
    // NOT an abort()/assert() — searched engine/ and game/ for an existing
    // "debug-build critical-invariant" abort pattern to reuse (as originally
    // requested) and found none (only vendored third-party Jolt/glm/doctest
    // call std::abort); CLAUDE.md's own hard rule is "no assert()" project-
    // wide. Also: this project's documented default build is
    // `-DCMAKE_BUILD_TYPE=Release` (confirmed via build/CMakeCache.txt), so
    // any #ifndef NDEBUG-gated abort would silently never fire in the
    // configuration actually used day to day — discovered directly while
    // building Point 1's guard against this same build. MD_LOG_ERROR is
    // always-on (no NDEBUG gate) and loud enough to be noticed without
    // violating either constraint.
    for (int pi = 0; pi < count_; ++pi) {
        const Entry& P = entries_[pi];
        for (int ri = 0; ri < P.desc.read_count; ++ri) {
            uint32_t res = P.desc.reads[ri];
            int writer_idx = -1;
            for (int wi = 0; wi < count_; ++wi) {
                const Entry& W = entries_[wi];
                for (int k = 0; k < W.desc.write_count; ++k)
                    if (W.desc.writes[k] == res) writer_idx = wi;  // take latest
            }
            if (writer_idx < 0 || writer_idx == pi) continue;  // no writer, or self (RMW)
            if (writer_idx > pi) {
                MD_LOG(MD_LOG_ERROR,
                       "[JobGraph] '%s' reads resource 0x%08x but writer '%s' "
                       "is registered AFTER it", P.name, res, entries_[writer_idx].name);
            }
        }
    }

    // Wave assignment: wave[i] = 1 + max(wave[j]) over every j < i this
    // batch MustSerialize() with, or 0 if none. Same-wave batches have no
    // declared conflict with each other or with anything in an earlier
    // wave, so they're independent enough to run concurrently (see the
    // header's structural-op caveat — declared-independent is necessary,
    // not sufficient, for a batch that touches MdRegistry directly).
    //
    // Point 2: MustSerialize() adds allows_concurrent==false as an
    // UNCONDITIONAL serialization requirement, on top of Conflicts()'s
    // tag-overlap check — a batch that performs unstaged structural ECS
    // ops must never share a wave with anything, regardless of tags.
    auto must_serialize = [](const JobBatchDesc& a, const JobBatchDesc& b) {
        return !a.allows_concurrent || !b.allows_concurrent || Conflicts(a, b);
    };
    int wave[MAX_BATCHES] = {};
    int max_wave = 0;
    for (int i = 0; i < count_; ++i) {
        int w = 0;
        for (int j = 0; j < i; ++j)
            if (must_serialize(entries_[j].desc, entries_[i].desc) && wave[j] + 1 > w)
                w = wave[j] + 1;
        wave[i] = w;
        if (w > max_wave) max_wave = w;
    }

    // Run wave by wave: a wave with 2+ batches goes through JobSystem
    // (each batch's fn() runs on a worker thread, same contract as any
    // other JobSystem::Submit caller — no registry access mid-flight
    // unless hand-verified safe); a lone batch just runs inline, same
    // NumWorkers()==0 fallback shape used elsewhere (e.g. logic_tick.cpp's
    // TickNavigation) so test binaries without JobSystem::Init() still work.
    for (int w = 0; w <= max_wave; ++w) {
#if defined(MD_ECS_GAIA)
        // Concurrency audit (2026-09-08, task #47/#48): gaia's
        // World::lock()/unlock() (m_structuralChangesLocked) is a plain
        // non-atomic uint32_t, incremented/decremented with ZERO
        // synchronization by EVERY .each() call (both the Iter&-callback
        // form MdEach uses today and the typed form), regardless of which
        // components a query touches. TSan-confirmed real data race + a
        // real `Assertion (m_structuralChangesLocked > 0)` crash when two
        // threads call .each() concurrently on the same World — even on
        // fully disjoint, correctly pre-warmed, Table-only queries with
        // zero Sparse components involved. Reproduced both in a
        // standalone minimal probe and on the real engine build via
        // test_job_graph_stress.cpp (80 TSan reports + a real abort).
        // Contradicts gaia's own README, which claims parallel query/
        // system execution within one World is safe — filed upstream as
        // a bug, not patched locally (no fork of the vendored
        // single-header). Every OTHER JobSystem::Submit call site in this
        // codebase (gpu_hal_buffers_dds_array.cpp, logic_tick_needs_ai_
        // nav.cpp's eval_nav_waypoint_job, npc_render_frame_prep.cpp's
        // eval_t2) is deliberately gather-job-scatter with ZERO Registry
        // access from the worker thread — this concurrent-dispatch branch
        // was the ONLY exception to that discipline in the whole
        // codebase, not a precedent for it. Also matches
        // prompt_/PROMPT_GAIA_MIGRATION.md §6's own stated non-goal:
        // "Не очікуй виграшу в конкурентності... Мета фази — паритет, не
        // прискорення" — there is no speedup being given up here.
        //
        // gaia therefore ALWAYS runs every wave sequentially, regardless
        // of wave_count — this is not a temporary flag pending an
        // upstream fix, it brings the gaia path in line with the
        // gather-job-scatter discipline every other worker-thread caller
        // in this codebase already follows. Wave computation above stays
        // shared with the flecs backend (still useful for the
        // read-after-write registration-order check); only EXECUTION
        // differs.
        for (int i = 0; i < count_; ++i)
            if (wave[i] == w) entries_[i].fn(entries_[i].user);
#else
        int wave_count = 0;
        for (int i = 0; i < count_; ++i) if (wave[i] == w) ++wave_count;

        if (wave_count > 1 && JobSystem::Get().NumWorkers() > 0) {
            // Real concurrent dispatch, per the probe-verified pattern in
            // md_registry.h: queries used by MdView::each() were already
            // built once against the main world (function-local statics),
            // so it's safe to enter readonly_begin(true) here and hand each
            // batch its own stage — MdRegistryStageScope (set inside
            // StagedThunk) makes every View<T>().each() in that batch's
            // call tree route through .iter(stage) instead of the raw
            // world for the duration of this wave.
            flecs::world& raw = Registry::Get();
            raw.set_stage_count(wave_count);
            raw.readonly_begin(true);
            int slot = 0;
            for (int i = 0; i < count_; ++i) {
                if (wave[i] != w) continue;
                StagedJob& job = g_staged_jobs[slot];
                job.fn    = entries_[i].fn;
                job.user  = entries_[i].user;
                job.stage = raw.get_stage(slot);
                JobSystem::Get().Submit(StagedThunk, &job);
                ++slot;
            }
            JobSystem::Get().Flush();
            raw.readonly_end();
            // Phase 4.4 (audit): release each used slot's stage claim right
            // after this wave, instead of leaving it in the global
            // g_staged_jobs array until the next Run() reassigns it (or,
            // worst case, until process exit). ASan caught a real
            // heap-use-after-free at teardown: a later, unrelated
            // set_stage_count() call elsewhere (e.g. a test manually
            // constructing its own stage) can resize/reallocate the main
            // world's internal stage array, invalidating a stale claim this
            // array was still holding from a previous wave — flecs::world's
            // move-assignment (world_t* -> nullptr) properly calls release()
            // first (see flecs.h's world::operator=(world&&)), so this is a
            // real fix, not just quieting the sanitizer.
            for (int i = 0; i < slot; ++i) g_staged_jobs[i].stage = flecs::world(nullptr);
        } else {
            for (int i = 0; i < count_; ++i)
                if (wave[i] == w) entries_[i].fn(entries_[i].user);
        }
#endif
    }

    count_ = 0;
}

} // namespace md

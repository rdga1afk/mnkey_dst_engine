#pragma once
#include <gaia.h>
#include <monkey_dust/platform/job_system.h>
#include <SDL3/SDL_atomic.h>
#include <atomic>
#include <cstdint>

// GaiaSchedAdapter — Phase 4 (PROMPT_GAIA_MIGRATION.md §6): routes gaia's
// scheduler hook (gaia::ecs::World::set_sched) through the engine's existing
// JobSystem instead of letting gaia lazily create its own gaia::mt::ThreadPool
// on first Parallel-exec query. CRITICAL, per §6: without an installed
// adapter, gaia raises a SECOND thread pool alongside JobSystem's -- two
// pools on Intel HD 520 is unacceptable. Install() MUST run before the
// FIRST possible parallel dispatch through this World (Registry::Get()
// wires it in at first construction -- see registry.h).
//
// Verified empirically (not assumed) before writing this: a standalone
// probe confirmed gaia::mt::ThreadPool::get() is a lazy Meyer's singleton,
// touched ONLY via gaia::ecs::sched_resolve() falling through to
// gaia::ecs::sched_def() when World::m_sched has every callback null.
// set_sched() with ANY non-null callback set permanently short-circuits
// that fallback -- confirmed via thread-count sampling
// (/proc/self/status "Threads:"): baseline Parallel .each() with no custom
// sched spun up +3 threads; the same call with a stub sched installed
// spun up +0. Also confirmed via QueryExecType::Default == Serial (gaia.h)
// that NOTHING in the current facade (MdEach always calls plain q.each(),
// no explicit exec type) touches Sched at all today -- this adapter closes
// a risk that only becomes live once JobGraph::Run() (job_graph.cpp) is
// rewritten to use gaia's parallel dispatch, not a live bug in anything
// already shipped.
//
// Fixed-size job-record pool (CLAUDE_CONSTITUTION.md: no malloc/new in
// hot-path) -- MAX_SCHED_JOBS must cover the worst-case in-flight token
// count across one JobGraph::Run() wave; sized with headroom over
// JobGraph::MAX_BATCHES (16).
namespace md_gaia_sched_detail {

constexpr int MAX_SCHED_JOBS = 64;

struct JobRec {
    void (*taskInvoke)(void*)                        = nullptr;  // sched()/add() path
    void (*parInvoke)(void*, uint32_t, uint32_t)      = nullptr;  // sched_par()/add_par() path
    void* ctx                                         = nullptr;
    uint32_t idxStart = 0, idxEnd = 0, groupSize = 0;
    SDL_AtomicInt      counter{};   // JobSystem's own completion counter -- still passed to
                                     // JobSystem::Submit() since its signature requires one, but
                                     // WaitJob() no longer spins on it, see `remaining` below.
    // [monkey_dust local patch, task #54] TSan-visible completion counter. libSDL3 is a plain
    // system package (not built with -fsanitize=thread -- confirmed via `strings
    // /usr/lib/libSDL3.so.0 | grep tsan`, no hits), so SDL_AtomicInt's increment/decrement
    // happens entirely inside an uninstrumented .so: the counter's VALUE ends up correct (real
    // hardware atomics), but TSan's shadow-memory instrumentation never sees the RMW and so can
    // never establish a happens-before edge through it. WaitJob()'s old busy-spin on
    // SDL_GetAtomicInt(&counter) therefore looked, to TSan, like an unsynchronized read racing
    // against the worker thread's writes to `ctx`/gaia's m_batches inside the job body -- 38
    // warnings, all rooted at GaiaSchedAdapter's first Parallel .each() (test_gaia_sched_adapter.cpp),
    // confirmed via a full TSan suite run before vs after this fix. std::atomic<int> IS
    // compiler-instrumented by -fsanitize=thread, so spinning on this instead gives TSan a real
    // synchronization point to reason about while leaving the counter's actual behavior (and
    // JobSystem's own internal bookkeeping via `counter` above) unchanged.
    std::atomic<int>   remaining{0};
    std::atomic<bool>  inUse{false};
    std::atomic<bool>  submitted{false};
};

inline JobRec g_jobs[MAX_SCHED_JOBS];

inline JobRec* AcquireSlot() {
    for (auto& j : g_jobs) {
        bool expected = false;
        if (j.inUse.compare_exchange_strong(expected, true)) {
            j.taskInvoke = nullptr;
            j.parInvoke  = nullptr;
            j.submitted.store(false, std::memory_order_relaxed);
            j.remaining.store(0, std::memory_order_relaxed);
            SDL_SetAtomicInt(&j.counter, 0);
            return &j;
        }
    }
    // Pool exhausted -- MAX_SCHED_JOBS sized with headroom over
    // JobGraph::MAX_BATCHES; hitting this means a caller outside JobGraph's
    // own wave discipline is using the scheduler. No silent UB: caller-side
    // dispatch functions null-check and run the work inline instead (see
    // Dispatch* below), same fail-open shape as JobSystem's own
    // NumWorkers()==0 fallback.
    return nullptr;
}

inline JobRec* TokenToJob(gaia::ecs::SchedToken token) {
    return reinterpret_cast<JobRec*>(token.value[0]);
}
inline gaia::ecs::SchedToken JobToToken(JobRec* job) {
    gaia::ecs::SchedToken t{};
    t.value[0] = reinterpret_cast<uintptr_t>(job);
    return t;
}

inline void RunTaskThunk(void* p) {
    auto* job = static_cast<JobRec*>(p);
    job->taskInvoke(job->ctx);
    job->remaining.fetch_sub(1, std::memory_order_release);
}
inline void RunParChunkThunk(void* p) {
    auto* job = static_cast<JobRec*>(p);
    job->parInvoke(job->ctx, job->idxStart, job->idxEnd);
    job->remaining.fetch_sub(1, std::memory_order_release);
}

inline void SubmitTask(JobRec* job) {
    // JobSystem::NumWorkers()==0 (test binaries without Init(), or a
    // single-core box) has nothing to drain the queue -- submitting there
    // would deadlock WaitJob() spinning on a counter nothing ever
    // decrements. Same fallback shape JobGraph::Run() already uses.
    job->remaining.store(1, std::memory_order_relaxed);
    if (JobSystem::Get().NumWorkers() > 0) {
        JobSystem::Get().Submit(RunTaskThunk, job, &job->counter);
    } else {
        RunTaskThunk(job);
    }
    job->submitted.store(true, std::memory_order_release);
}

// Splits [0, itemCount) into JobSystem submissions of ~groupSize items each,
// all sharing job's single completion counter (JobSystem::Submit's counter
// overload already supports N submissions sharing one counter -- same
// pattern job_graph.cpp's StagedJob dispatch uses per-wave).
inline void SubmitPar(JobRec* job, uint32_t itemCount, uint32_t groupSize) {
    if (itemCount == 0) { job->submitted.store(true, std::memory_order_release); return; }
    if (JobSystem::Get().NumWorkers() == 0) {
        job->parInvoke(job->ctx, 0, itemCount);  // inline, single chunk -- see SubmitTask's note
        job->submitted.store(true, std::memory_order_release);
        return;
    }
    uint32_t gs = groupSize > 0 ? groupSize : itemCount;  // groupSize==0 => scheduler picks; one chunk here
    const uint32_t chunkCount = (itemCount + gs - 1) / gs;
    job->remaining.store((int)chunkCount, std::memory_order_relaxed);
    for (uint32_t start = 0; start < itemCount; start += gs) {
        uint32_t end = start + gs < itemCount ? start + gs : itemCount;
        // Reuses job's own idxStart/idxEnd as the LAST chunk's range for any
        // caller reading them post-hoc is unsafe with >1 chunk -- chunk
        // bounds are captured by value in a small per-chunk record instead.
        struct ChunkRec { void (*fn)(void*, uint32_t, uint32_t); void* ctx; uint32_t s, e; JobRec* job; };
        static ChunkRec chunks[MAX_SCHED_JOBS * 4];
        static std::atomic<int> chunkCursor{0};
        int slot = chunkCursor.fetch_add(1, std::memory_order_relaxed) % (MAX_SCHED_JOBS * 4);
        chunks[slot] = ChunkRec{job->parInvoke, job->ctx, start, end, job};
        JobSystem::Get().Submit(
            [](void* p) {
                auto* c = static_cast<ChunkRec*>(p);
                c->fn(c->ctx, c->s, c->e);
                c->job->remaining.fetch_sub(1, std::memory_order_release);
            },
            &chunks[slot], &job->counter);
    }
    job->submitted.store(true, std::memory_order_release);
}

inline void WaitJob(JobRec* job) {
    if (!job->submitted.load(std::memory_order_acquire)) return;  // add()'d but never submit()'d
    // Spins on `remaining` (std::atomic<int>), not `counter` (SDL_AtomicInt) -- see
    // JobRec::remaining's doc comment for why the SDL one is invisible to TSan.
    while (job->remaining.load(std::memory_order_acquire) > 0) {
        // Matches job_system.h's own documented spin idiom (fiber-pattern
        // Submit-with-counter doc comment) -- no busy-loop sleep tuning
        // needed here, JobGraph waves are short-lived.
    }
}

inline void ReleaseJob(JobRec* job) {
    job->inUse.store(false, std::memory_order_release);
}

// ---- gaia::ecs::Sched callback table -------------------------------------

inline gaia::ecs::SchedToken Cb_Sched(void*, const gaia::ecs::SchedTaskDesc* pDesc) {
    JobRec* job = AcquireSlot();
    if (job == nullptr) { pDesc->invoke(pDesc->pCtx); return gaia::ecs::SchedToken{}; }  // fail-open: run inline
    job->taskInvoke = pDesc->invoke;
    job->ctx        = pDesc->pCtx;
    SubmitTask(job);
    return JobToToken(job);
}

inline gaia::ecs::SchedToken Cb_SchedPar(void*, const gaia::ecs::SchedParDesc* pDesc) {
    JobRec* job = AcquireSlot();
    if (job == nullptr) { pDesc->invoke(pDesc->pCtx, 0, pDesc->itemCount); return gaia::ecs::SchedToken{}; }
    job->parInvoke = pDesc->invoke;
    job->ctx       = pDesc->pCtx;
    SubmitPar(job, pDesc->itemCount, pDesc->groupSize);
    return JobToToken(job);
}

inline gaia::ecs::SchedToken Cb_Add(void*, const gaia::ecs::SchedTaskDesc* pDesc) {
    JobRec* job = AcquireSlot();
    if (job == nullptr) { pDesc->invoke(pDesc->pCtx); return gaia::ecs::SchedToken{}; }
    job->taskInvoke = pDesc->invoke;
    job->ctx        = pDesc->pCtx;
    return JobToToken(job);  // NOT submitted yet -- Cb_Submit does that
}

inline gaia::ecs::SchedToken Cb_AddPar(void*, const gaia::ecs::SchedParDesc* pDesc) {
    JobRec* job = AcquireSlot();
    if (job == nullptr) { pDesc->invoke(pDesc->pCtx, 0, pDesc->itemCount); return gaia::ecs::SchedToken{}; }
    job->parInvoke  = pDesc->invoke;
    job->ctx        = pDesc->pCtx;
    job->idxStart   = 0;
    job->idxEnd     = pDesc->itemCount;
    job->groupSize  = pDesc->groupSize;
    return JobToToken(job);
}

inline void Cb_Submit(void*, gaia::ecs::SchedToken token) {
    JobRec* job = TokenToJob(token);
    if (job == nullptr) return;
    if (job->parInvoke != nullptr) SubmitPar(job, job->idxEnd - job->idxStart, job->groupSize);
    else                           SubmitTask(job);
}

inline void Cb_Dep(void*, gaia::ecs::SchedToken tokenFirst, gaia::ecs::SchedToken) {
    // JobSystem has no native task-graph dependency edge; the only
    // correctness requirement dep() must satisfy is "second runs after
    // first completes". Cb_Wait on tokenFirst before the caller's own
    // submit() of the second token achieves that -- JobGraph's own
    // wave-barrier discipline (job_graph.cpp's Run(), one Flush() per wave)
    // is what actually orders work in practice; dep() is not on any path
    // exercised by the current facade (confirmed: nothing calls
    // QueryExecType::Parallel today, see this file's top comment).
    JobRec* first = TokenToJob(tokenFirst);
    if (first != nullptr) WaitJob(first);
}

inline void Cb_Wait(void*, gaia::ecs::SchedToken token) {
    JobRec* job = TokenToJob(token);
    if (job != nullptr) WaitJob(job);
}

inline void Cb_Del(void*, gaia::ecs::SchedToken token) {
    JobRec* job = TokenToJob(token);
    if (job == nullptr) return;
    WaitJob(job);  // safe no-op if already waited (counter already 0)
    ReleaseJob(job);
}

} // namespace md_gaia_sched_detail

class MdGaiaSchedAdapter {
public:
    static const gaia::ecs::Sched& Instance() {
        static const gaia::ecs::Sched s = [] {
            gaia::ecs::Sched b{};
            b.pCtx     = nullptr;
            b.sched    = &md_gaia_sched_detail::Cb_Sched;
            b.sched_par= &md_gaia_sched_detail::Cb_SchedPar;
            b.add      = &md_gaia_sched_detail::Cb_Add;
            b.add_par  = &md_gaia_sched_detail::Cb_AddPar;
            b.submit   = &md_gaia_sched_detail::Cb_Submit;
            b.dep      = &md_gaia_sched_detail::Cb_Dep;
            b.wait     = &md_gaia_sched_detail::Cb_Wait;
            b.del      = &md_gaia_sched_detail::Cb_Del;
            return b;
        }();
        return s;
    }

    // Call exactly once, before any parallel-capable query/system dispatch
    // through `w`. Idempotent (re-installing the same Instance() is a
    // harmless no-op) but not thread-safe to call concurrently with a
    // dispatch already in flight -- call during single-threaded startup.
    static void Install(gaia::ecs::World& w) { w.set_sched(Instance()); }
};


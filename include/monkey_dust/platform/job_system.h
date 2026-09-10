#pragma once
#include <SDL3/SDL_thread.h>
#include <SDL3/SDL_mutex.h>
#include <cstdint>

// VBfA-pattern job queue: CPU_count-1 worker threads, 100-slot circular buffer.
//
// Constants from VBfA binary (viking.exe.c):
//   max_queued_jobs = 100    (line 390933)
//   worker_threads  = CPU-1  (line 390815)
//   stack_per_thread = 64KB  (line 390792)
//
// Thread safety:
//   Submit() — main thread only during frame.
//   Flush()  — main thread; blocks until all in-flight jobs complete.
//   Workers  — call fn(data) concurrently; jobs must not touch EnTT registry
//              or GPU resources. Safe for: animation eval (T2 tier), path eval,
//              sense distance queries.
//
// Usage:
//   JobSystem::Get().Init();          // once at startup
//   JobSystem::Get().Submit(fn, ptr); // during frame
//   JobSystem::Get().Flush();         // before reading results / uploading GPU
//   JobSystem::Get().Shutdown();      // at exit
struct JobSystem {
    static constexpr int MAX_JOBS    = 100;
    static constexpr int MAX_WORKERS = 15;  // hard cap; actual = min(CPU-1, MAX_WORKERS)

    struct Job {
        void (*fn)(void*);
        void* data;
        SDL_AtomicInt* counter = nullptr;  // optional; decremented on completion
    };

    // CATHODE RE §7.5: 7-level thread priority table (AI.exe lines 1818006-1818027).
    // Indexed by WorkerRole enum. Configured via tasks.cfg or SetWorkerPriority().
    enum WorkerRole : int {
        RoleRender    = 0,  // TIME_CRITICAL  — frame-rate sensitive
        RolePhysics   = 1,  // ABOVE_NORMAL
        RoleAI        = 2,  // ABOVE_NORMAL
        RoleAnimation = 3,  // NORMAL         — default game logic
        RoleStreaming  = 4,  // BELOW_NORMAL
        RoleBackground= 5,  // BELOW_NORMAL
        RoleIdle      = 6,  // IDLE           — background precompute
        RoleCount     = 7
    };
    // SDL thread priority mapping per role (SDL_ThreadPriority values):
    // TIME_CRITICAL=3, HIGH=2, NORMAL=1, LOW=0
    static constexpr int kPriorityTable[RoleCount] = {
        3,  // Render    → SDL_THREAD_PRIORITY_TIME_CRITICAL
        2,  // Physics   → SDL_THREAD_PRIORITY_HIGH
        2,  // AI        → SDL_THREAD_PRIORITY_HIGH
        1,  // Animation → SDL_THREAD_PRIORITY_NORMAL
        0,  // Streaming → SDL_THREAD_PRIORITY_LOW
        0,  // Background→ SDL_THREAD_PRIORITY_LOW
        0,  // Idle      → SDL_THREAD_PRIORITY_LOW
    };

    // 3-tier convenience alias — RE-confirmed: Kenshi uses sim/worker/background tiers.
    // Maps to WorkerRole: HIGH=physics/BT, NORMAL=animation, LOW=pathfinding/streaming.
    enum class JobPriority : int {
        HIGH   = RolePhysics,    // BT update, physics step
        NORMAL = RoleAnimation,  // animation sampling, sense queries
        LOW    = RoleStreaming,   // pathfinding, chunk I/O
    };

    // Assign a role to worker thread i (applied at Init if called before Init,
    // or immediately if called after). workers 0..N-1, default role = RoleAI.
    void SetWorkerRole(int worker_idx, WorkerRole role);

    // VBfA RE: batch_size config key (Tasks.txt line 43185).
    int BatchSize() const { return batch_size_; }

    static JobSystem& Get();

    // Load worker_threads and batch_size from a simple key=value config file.
    // Format (VBfA Tasks.txt compatible):
    //   worker_threads 4
    //   batch_size 16
    // Call before Init(). Missing keys keep defaults (CPU-1, 16).
    void LoadFromCfg(const char* path);

    void Init();
    void Shutdown();

    // Submit a job. Blocks briefly if queue is full (back-pressure, rare in practice).
    void Submit(void (*fn)(void*), void* data);

    // Submit with dependency counter — decrements counter atomically on completion.
    // Naughty Dog fiber pattern: instead of blocking Flush(), caller spins on counter.
    // Usage:
    //   SDL_atomic_t ctr = {0};
    //   for (...) { SDL_AtomicAdd(&ctr, 1); Submit(fn, data, &ctr); }
    //   while (SDL_AtomicGet(&ctr) > 0) _mm_pause();  // or SDL_Delay(0) if sleeping
    void Submit(void (*fn)(void*), void* data, SDL_AtomicInt* counter);

    // Wait until every submitted job has completed.
    void Flush();

    int  NumWorkers() const { return num_workers_; }

    // Audit Phase 2b (TSan falsification pair, AUDIT_PLAN_v1.md): forces every
    // Submit() to run its job synchronously on the calling thread instead of
    // queueing it to a worker. Wired to --jobgraph-serial (scenario_driver.cpp)
    // — running the same scenario once with this OFF (default, real parallel
    // dispatch) and once with it ON under TSan, a race report present only in
    // the OFF run is a real data race in job-parallel code; one present in
    // BOTH runs is a lower-layer issue (allocator, logging, etc.) unrelated to
    // this job system's own parallelism. Call before Init() or any Submit().
    void SetForceSerial(bool v) { force_serial_ = v; }
    bool ForceSerial() const { return force_serial_; }

private:
    // TSan-confirmed data race (2026-09-08, task #50): SDL_CreateThread()
    // starts the new thread immediately, before Init()'s loop finishes
    // writing threads_[] for later indices -- an already-running worker's
    // self-identification scan (the old worker_loop() body) read threads_[]
    // concurrently with Init()'s writes to it. Fixed by handing each worker
    // its own identity directly at spawn time instead of making it search a
    // shared array for itself -- no shared mutable state between Init()'s
    // loop and a just-started worker at all, not just a narrower window.
    struct WorkerCtx { JobSystem* js; int idx; };
    static int SDLCALL s_worker_entry(void* ctx);
    void worker_loop(int my_idx);

    WorkerCtx     worker_ctxs_[MAX_WORKERS] = {};
    Job           buf_[MAX_JOBS]         = {};
    int           head_                  = 0;
    int           tail_                  = 0;
    int           count_                 = 0;  // jobs in queue
    int           inflight_              = 0;  // jobs queued + executing
    bool          quit_                  = false;
    bool          force_serial_          = false;
    int           num_workers_           = 0;
    int           batch_size_            = 16;
    int           cfg_worker_override_   = 0;
    WorkerRole    worker_roles_[MAX_WORKERS] = {};  // per-worker role; default RoleAI(2)

    SDL_Mutex*     mtx_      = nullptr;
    SDL_Condition* cv_work_  = nullptr;  // workers wait here
    SDL_Condition* cv_done_  = nullptr;  // Flush() waits here
    SDL_Condition* cv_cap_   = nullptr;  // Submit() waits if full
    SDL_Thread*    threads_[MAX_WORKERS] = {};
};

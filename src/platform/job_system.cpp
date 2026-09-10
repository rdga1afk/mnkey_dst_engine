#include <monkey_dust/platform/job_system.h>
#include <SDL3/SDL_cpuinfo.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <monkey_dust/platform/md_log.h>
#ifdef __linux__
#  include <pthread.h>
#  include <SDL3/SDL_thread.h>
#endif

JobSystem& JobSystem::Get() {
    static JobSystem s;
    return s;
}

// ── Worker thread entry ───────────────────────────────────────────────────────

int SDLCALL JobSystem::s_worker_entry(void* ctx) {
    auto* c = static_cast<WorkerCtx*>(ctx);
    c->js->worker_loop(c->idx);
    return 0;
}

void JobSystem::worker_loop(int my_idx) {
    // SDL_SetCurrentThreadPriority sets the *calling* thread's priority.
    // my_idx is handed directly by s_worker_entry (see WorkerCtx's doc
    // comment) -- no shared-array self-identification scan, so nothing
    // here can race with Init()'s loop writing threads_[]/worker_ctxs_[]
    // for other indices.
    {
        int prio = kPriorityTable[(int)worker_roles_[my_idx]];
        SDL_SetCurrentThreadPriority((SDL_ThreadPriority)prio);
    }

    while (true) {
        SDL_LockMutex(mtx_);
        while (!quit_ && count_ == 0)
            SDL_WaitCondition(cv_work_, mtx_);
        if (quit_) { SDL_UnlockMutex(mtx_); return; }

        Job job   = buf_[tail_];
        tail_     = (tail_ + 1) % MAX_JOBS;
        --count_;
        SDL_BroadcastCondition(cv_cap_);  // slot freed — unblock a waiting Submit
        SDL_UnlockMutex(mtx_);

        job.fn(job.data);
        // Naughty Dog fiber pattern: decrement dependency counter if set.
        if (job.counter) SDL_AtomicDecRef(job.counter);

        SDL_LockMutex(mtx_);
        if (--inflight_ == 0)
            SDL_BroadcastCondition(cv_done_);
        SDL_UnlockMutex(mtx_);
    }
}

// ── Public API ────────────────────────────────────────────────────────────────

// ── Config loader (VBfA Tasks.txt format) ────────────────────────────────────
void JobSystem::LoadFromCfg(const char* path) {
    FILE* f = ::fopen(path, "r");
    if (!f) {
        MD_LOG(MD_LOG_WARNING, "[JobSystem] tasks.cfg not found at '%s', using defaults", path);
        return;
    }
    char line[128];
    while (fgets(line, sizeof(line), f)) {
        if (line[0] == '#' || line[0] == '\n') continue;
        char key[64]; int val = 0;
        if (sscanf(line, "%63s %d", key, &val) == 2) {
            if (strcmp(key, "worker_threads") == 0 && val > 0)
                cfg_worker_override_ = val;
            else if (strcmp(key, "batch_size") == 0 && val > 0)
                batch_size_ = val;
        }
    }
    fclose(f);
    MD_LOG(MD_LOG_INFO, "[JobSystem] tasks.cfg: worker_threads=%d batch_size=%d",
           cfg_worker_override_, batch_size_);
}

constexpr int JobSystem::kPriorityTable[JobSystem::RoleCount];

void JobSystem::SetWorkerRole(int idx, WorkerRole role) {
    if (idx < 0 || idx >= MAX_WORKERS) return;
    worker_roles_[idx] = role;
    if (idx < num_workers_ && threads_[idx]) {
        // Apply immediately if thread is running.
        // SDL_SetThreadPriority sets priority of calling thread;
        // we can only hint — actual change requires thread cooperation.
        (void)role; // priority applied at thread spawn; runtime change not supported on all platforms
    }
}

void JobSystem::Init() {
    // task #54: Init() must be idempotent. test_gaia_sched_adapter.cpp calls
    // Init() unconditionally (no NumWorkers()==0 guard) to assert steady-state
    // behavior; since JobSystem is a process-wide singleton and other tests
    // (test_job_graph_stress.cpp) deliberately never call Shutdown(), a second
    // Init() call was overwriting mtx_/cv_work_/cv_done_/cv_cap_ and spawning a
    // duplicate worker set while the FIRST set's threads were still alive and
    // reading the now-clobbered mtx_ pointer on every worker_loop() iteration
    // -- a genuine (TSan-confirmed) data race on job_system.cpp's plain pointer
    // members, not a false positive. Early-return once workers already exist.
    if (num_workers_ > 0)
        return;

    mtx_     = SDL_CreateMutex();
    cv_work_ = SDL_CreateCondition();
    cv_done_ = SDL_CreateCondition();
    cv_cap_  = SDL_CreateCondition();

    int cores = SDL_GetNumLogicalCPUCores();
    if (cfg_worker_override_ > 0)
        num_workers_ = std::min(cfg_worker_override_, MAX_WORKERS);
    else
        num_workers_ = std::min(std::max(cores - 1, 1), MAX_WORKERS);

    for (int i = 0; i < num_workers_; ++i) {
        char name[16];
        SDL_snprintf(name, sizeof(name), "JobWorker%d", i);
        // worker_ctxs_[i] written BEFORE SDL_CreateThread spawns the thread
        // that reads it -- see WorkerCtx's doc comment in job_system.h.
        worker_ctxs_[i] = WorkerCtx{this, i};
        threads_[i] = SDL_CreateThread(s_worker_entry, name, &worker_ctxs_[i]);

        // Role applied inside worker_loop() via my_idx (handed directly, no
        // scan) — worker calls SDL_SetCurrentThreadPriority at startup.

#ifdef __linux__
        {
            pthread_t tid = (pthread_t)SDL_GetThreadID(threads_[i]);
            cpu_set_t cs;
            CPU_ZERO(&cs);
            CPU_SET((i + 1) % cores, &cs);
            pthread_setaffinity_np(tid, sizeof(cs), &cs);
        }
#endif
    }
    MD_LOG(MD_LOG_INFO, "[JobSystem] %d workers, batch_size=%d (SDL %d logical cores)",
           num_workers_, batch_size_, cores);
}

void JobSystem::Shutdown() {
    SDL_LockMutex(mtx_);
    quit_ = true;
    SDL_BroadcastCondition(cv_work_);
    SDL_UnlockMutex(mtx_);

    for (int i = 0; i < num_workers_; ++i)
        SDL_WaitThread(threads_[i], nullptr);

    SDL_DestroyMutex(mtx_);
    SDL_DestroyCondition(cv_work_);
    SDL_DestroyCondition(cv_done_);
    SDL_DestroyCondition(cv_cap_);
}

void JobSystem::Submit(void (*fn)(void*), void* data) {
    if (force_serial_) { fn(data); return; }
    SDL_LockMutex(mtx_);
    while (count_ >= MAX_JOBS)           // back-pressure: queue full
        SDL_WaitCondition(cv_cap_, mtx_);
    buf_[head_] = {fn, data};
    head_       = (head_ + 1) % MAX_JOBS;
    ++count_;
    ++inflight_;
    SDL_BroadcastCondition(cv_work_);
    SDL_UnlockMutex(mtx_);
}

void JobSystem::Flush() {
    SDL_LockMutex(mtx_);
    while (inflight_ > 0)
        SDL_WaitCondition(cv_done_, mtx_);
    SDL_UnlockMutex(mtx_);
}

// Naughty Dog fiber pattern: submit with dependency counter.
// Counter is incremented here, decremented atomically when job completes.
// Caller can spin-wait on counter instead of blocking Flush().
void JobSystem::Submit(void (*fn)(void*), void* data, SDL_AtomicInt* counter) {
    if (force_serial_) {
        SDL_AtomicIncRef(counter);
        fn(data);
        SDL_AtomicDecRef(counter);
        return;
    }
    SDL_LockMutex(mtx_);
    while (count_ >= MAX_JOBS)
        SDL_WaitCondition(cv_cap_, mtx_);
    SDL_AtomicIncRef(counter);
    buf_[head_] = {fn, data, counter};
    head_ = (head_ + 1) % MAX_JOBS;
    ++count_;
    ++inflight_;
    SDL_BroadcastCondition(cv_work_);
    SDL_UnlockMutex(mtx_);
}

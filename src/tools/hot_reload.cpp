#include <monkey_dust/tools/hot_reload.h>
#include <monkey_dust/platform/md_log.h>
#include <cstring>
#include <sys/stat.h>

#include <SDL3/SDL.h>

// ── mtime helper ─────────────────────────────────────────────────────────────

static int64_t file_mtime(const char* path) {
    struct stat st;
    if (stat(path, &st) != 0) return -1;
#if defined(_WIN32) || defined(__APPLE__)
    return (int64_t)st.st_mtime;
#else
    return (int64_t)st.st_mtim.tv_sec;
#endif
}

// ── Poll ─────────────────────────────────────────────────────────────────────

void HotReload::Poll() {
    for (int i = 0; i < count_; ++i) {
        WatchEntry& e = entries_[i];
        if (!e.path[0] || !e.fn) continue;

        int64_t mtime = file_mtime(e.path);
        if (mtime == -1) continue;  // file missing — no action

        if (e.last_mtime == -1) {
            e.last_mtime = mtime;   // first poll → baseline, no callback
            continue;
        }
        if (mtime != e.last_mtime) {
            e.last_mtime = mtime;
            MD_LOG(MD_LOG_INFO, "[HotReload] changed: %s", e.path);
            e.fn(e.path);
        }
    }
}

// ── Public API ────────────────────────────────────────────────────────────────

bool HotReload::Watch(const char* path, ReloadFunc fn) {
    // Check duplicate
    for (int i = 0; i < count_; ++i)
        if (strncmp(entries_[i].path, path, sizeof(entries_[0].path)) == 0) return true;

    if (count_ >= MAX_WATCHED) {
        MD_LOG(MD_LOG_WARNING, "[HotReload] max watched (%d) reached, ignoring '%s'", MAX_WATCHED, path);
        return false;
    }
    WatchEntry& e = entries_[count_++];
    strncpy(e.path, path, sizeof(e.path) - 1);
    e.fn         = fn;
    e.last_mtime = -1;  // will be set on first Poll
    MD_LOG(MD_LOG_INFO, "[HotReload] watching '%s'", path);
    return true;
}

void HotReload::Unwatch(const char* path) {
    for (int i = 0; i < count_; ++i) {
        if (strncmp(entries_[i].path, path, sizeof(entries_[0].path)) == 0) {
            entries_[i] = entries_[--count_];
            entries_[count_] = WatchEntry{};
            return;
        }
    }
}

void HotReload::PollOnce() {
    Poll();
}

// ── Threading (SDL3) ─────────────────────────────────────────────────────────

int HotReload::ThreadFunc(void* userdata) {
    HotReload* self = static_cast<HotReload*>(userdata);
    while (self->running_) {
        self->Poll();
        SDL_Delay(self->poll_ms_);
    }
    return 0;
}

void HotReload::Start(uint32_t poll_ms) {
    Stop();  // ensure no leftover thread
    poll_ms_ = poll_ms;
    running_ = true;
    thread_ = static_cast<void*>(SDL_CreateThread(ThreadFunc, "HotReload", this));
    if (!thread_) {
        running_ = false;
        MD_LOG(MD_LOG_WARNING, "[HotReload] SDL_CreateThread failed: %s", SDL_GetError());
    }
}

void HotReload::Stop() {
    if (!running_) return;
    running_ = false;
    if (thread_) {
        SDL_WaitThread(static_cast<SDL_Thread*>(thread_), nullptr);
        thread_ = nullptr;
    }
}

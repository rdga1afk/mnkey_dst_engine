#include <monkey_dust/scripting/lua_system.h>
#include <monkey_dust/platform/md_log.h>
#include <cstring>
#include <cstdint>
#include <monkey_dust/compat/md_dirent.h>

// Custom allocator: enforce LUA_MEM_LIMIT_BYTES.
// Returns nullptr when limit exceeded — Lua handles OOM gracefully.
//
// terrain-perf-measure (2026-08-12, task #406): the old `*used - osize +
// nsize` arithmetic is unsigned (size_t) and has no protection against
// `osize > *used` — proven live via a temp diagnostic (lua_gc(LUA_GCCOUNT)
// stayed flat at 28KB, real Lua heap usage, while *used spiraled DOWN
// ~42 bytes/call and underflowed to ~2^64 around call #640, after which
// every allocation permanently failed with a real but misleading "not
// enough memory"). Root cause of the per-call mismatch not fully isolated
// (likely: for a brand-new object Lua's alloc convention passes a small
// type-tag as `osize`, not a real previous size, which this counter
// treated as a real size to subtract — see Lua 5.4 manual §lua_Alloc), but
// regardless of the exact mismatch source, unsigned subtraction should
// never be allowed to wrap. Clamped arithmetic below makes that
// structurally impossible: worst case the tracked count drifts slightly
// inaccurate, never catastrophically wraps into a permanent-failure state.
void* LuaSystem::lua_alloc(void* ud, void* ptr, size_t osize, size_t nsize) {
    auto* used = static_cast<size_t*>(ud);
    if (nsize == 0) {
        *used = (osize <= *used) ? (*used - osize) : 0;
        free(ptr);
        return nullptr;
    }
    size_t grow = (nsize > osize) ? (nsize - osize) : 0;
    if (*used + grow > LUA_MEM_LIMIT_BYTES)
        return nullptr; // allocation refused → Lua raises MemError
    void* p = realloc(ptr, nsize);
    if (p) {
        if (nsize >= osize) *used += (nsize - osize);
        else                *used = (osize - nsize <= *used) ? (*used - (osize - nsize)) : 0;
    }
    return p;
}

void LuaSystem::hook(lua_State* L, lua_Debug* /*ar*/) {
    luaL_error(L, "[LuaSystem] instruction limit exceeded");
}

int LuaSystem::md_log(lua_State* L) {
    const char* msg = lua_tostring(L, 1);
    if (msg) MD_LOG(MD_LOG_INFO, "[Lua] %s", msg);
    return 0;
}

bool LuaSystem::LoadFile(const char* path) {
    if (luaL_dofile(L_, path) != LUA_OK) {
        const char* err = lua_tostring(L_, -1);
        MD_LOG(MD_LOG_WARNING, "[LuaSystem] Load error %s: %s",
                 path, err ? err : "?");
        lua_pop(L_, 1);
        return false;
    }
    MD_LOG(MD_LOG_INFO, "[LuaSystem] Loaded: %s", path);
    return true;
}

bool LuaSystem::Init(const char* scripts_dir) {
    lua_mem_used_ = 0;
#if LUA_VERSION_NUM >= 505
    // Lua 5.5+: lua_newstate API changed; use luaL_newstate() then override allocator
    L_ = luaL_newstate();
    if (L_) lua_setallocf(L_, lua_alloc, &lua_mem_used_);
#else
    L_ = lua_newstate(lua_alloc, &lua_mem_used_);
#endif
    if (!L_) return false;

    luaL_openlibs(L_);

    // Remove dangerous standard libraries — sandbox enforcement.
    // luaL_openlibs opens io/os/package/debug which allow shell exec and FS access.
    static const char* const blocked[] = {
        "io", "os", "package", "debug", nullptr
    };
    for (int i = 0; blocked[i]; ++i) {
        lua_pushnil(L_);
        lua_setglobal(L_, blocked[i]);
    }
    // Also block the load* family (can compile arbitrary bytecode)
    static const char* const blocked_fns[] = {
        "dofile", "loadfile", "load", "loadstring", nullptr
    };
    for (int i = 0; blocked_fns[i]; ++i) {
        lua_pushnil(L_);
        lua_setglobal(L_, blocked_fns[i]);
    }

    lua_register(L_, "md_log", md_log);
    lua_sethook(L_, hook, LUA_MASKCOUNT, 50000);

    // Scenario control-flow primitives (Etap 1, AUTONOMY_SYSTEM_PROMPT_v2.md).
    // Pure Lua, not C functions: wait_ticks/wait_until just need
    // coroutine.yield() (one yield = one logic tick, from the driver's
    // perspective in ResumeScenario); md.quit raises a distinguishable
    // error value (a table, not a string) so ResumeScenario can tell
    // "intentional quit" apart from a genuine script failure without any
    // extra C-side bookkeeping.
    static const char* const kScenarioBootstrap =
        "md = md or {}\n"
        "function md.wait_ticks(n)\n"
        "  for _ = 1, n do coroutine.yield() end\n"
        "end\n"
        "function md.wait_until(fn, timeout_ticks)\n"
        "  if timeout_ticks == nil then error('wait_until: timeout_ticks is required') end\n"
        "  for _ = 1, timeout_ticks do\n"
        "    if fn() then return true end\n"
        "    coroutine.yield()\n"
        "  end\n"
        "  error('wait_until: timed out after ' .. timeout_ticks .. ' ticks')\n"
        "end\n"
        "function md.quit(code)\n"
        "  error({__md_quit = true, code = code or 0})\n"
        "end\n";
    if (luaL_dostring(L_, kScenarioBootstrap) != LUA_OK) {
        const char* err = lua_tostring(L_, -1);
        MD_LOG(MD_LOG_WARNING, "[LuaSystem] scenario bootstrap failed: %s", err ? err : "?");
        lua_pop(L_, 1);
    }

    if (!scripts_dir || scripts_dir[0] == '\0') return true;

    DIR* dir = opendir(scripts_dir);
    if (!dir) {
        MD_LOG(MD_LOG_WARNING, "[LuaSystem] Cannot open scripts dir: %s", scripts_dir);
        return true;
    }
    struct dirent* ent;
    while ((ent = readdir(dir)) != nullptr) {
        const char* name = ent->d_name;
        size_t len = strlen(name);
        if (len > 4 && strcmp(name + len - 4, ".lua") == 0) {
            char path_buf[256];
            snprintf(path_buf, sizeof(path_buf), "%s/%s", scripts_dir, name);
            LoadFile(path_buf);
        }
    }
    closedir(dir);
    return true;
}

void LuaSystem::RegisterFunction(const char* name, lua_CFunction fn) {
    if (L_) lua_register(L_, name, fn);
}

void LuaSystem::RegisterNamespaceFunction(const char* field_name, lua_CFunction fn) {
    if (!L_) return;
    lua_getglobal(L_, "md");
    if (!lua_istable(L_, -1)) {
        lua_pop(L_, 1);
        lua_newtable(L_);
        lua_pushvalue(L_, -1);
        lua_setglobal(L_, "md");
    }
    lua_pushcfunction(L_, fn);
    lua_setfield(L_, -2, field_name);
    lua_pop(L_, 1);
}

void LuaSystem::RegisterSubNamespaceFunction(const char* sub_table, const char* field_name, lua_CFunction fn) {
    if (!L_) return;
    lua_getglobal(L_, "md");
    if (!lua_istable(L_, -1)) {
        lua_pop(L_, 1);
        lua_newtable(L_);
        lua_pushvalue(L_, -1);
        lua_setglobal(L_, "md");
    }
    lua_getfield(L_, -1, sub_table);
    if (!lua_istable(L_, -1)) {
        lua_pop(L_, 1);
        lua_newtable(L_);
        lua_pushvalue(L_, -1);
        lua_setfield(L_, -3, sub_table);
    }
    lua_pushcfunction(L_, fn);
    lua_setfield(L_, -2, field_name);
    lua_pop(L_, 2);
}

void LuaSystem::Shutdown() {
    if (L_) { lua_close(L_); L_ = nullptr; }
}

bool LuaSystem::Exec(const char* lua_code, char* error_out, size_t error_out_size) {
    if (!L_ || !lua_code) return false;
    lua_sethook(L_, hook, LUA_MASKCOUNT, 50000);
    // Explicit short ASCII chunkname ("cmd") instead of luaL_dostring's
    // default (the raw source text itself, truncated to a fixed byte
    // length by Lua's luaO_chunkid — confirmed live via task #123's
    // command-file channel: a non-ASCII literal (e.g. a Cyrillic
    // assert_true label) gets cut mid-UTF8-character at the truncation
    // point, producing an invalid-UTF8 error message that then fails to
    // parse as JSON/text downstream). The reported error TEXT itself
    // (from luaL_error) is unaffected — only the chunkname-echo prefix was
    // ever corrupted.
    //
    // terrain-perf-measure (2026-08-12): base = stack depth before this
    // call. LUA_MULTRET in the pcall below pushes however many values the
    // executed chunk returns (0 for a bare statement, but callers here are
    // arbitrary caller-supplied Lua, so never assume 0) — the old code only
    // popped on the error path (exactly 1 error object) and never touched
    // the success path, leaking one chunk's worth of return values onto L_
    // every call. lua_settop(L_, base) after either outcome restores the
    // pre-call depth unconditionally.
    //
    // That stack fix alone was NOT sufficient at first, but the extra
    // failures traced to an unrelated bug: an unsigned size_t underflow in
    // lua_alloc's own byte-accounting (see that function's doc comment) —
    // NOT actual GC pressure. Live diagnostic during that investigation
    // (lua_gc(L_, LUA_GCCOUNT, 0), the real Lua heap size) stayed flat at
    // ~28KB through a 3000-call stress test the whole time, confirming
    // Lua's normal incremental GC (auto-stepped by allocation pressure, no
    // manual stepping needed) already reclaims these throwaway per-Exec
    // closures on its own. An explicit lua_gc(LUA_GCCOLLECT) full sweep was
    // added here as a hypothesis while chasing that bug and is NOT needed
    // for correctness now that lua_alloc itself is fixed — removed, since
    // L_ is shared with the BT/dialogue Lua paths and a full mark-sweep on
    // every Exec() call (this channel is externally-driven, can run in a
    // tight loop during perf-measurement) would be unjustified cost on
    // that shared heap. lua_settop(L_, base) is the real, load-bearing fix
    // and stays on both paths unconditionally.
    int base = lua_gettop(L_);
    bool failed = luaL_loadbuffer(L_, lua_code, strlen(lua_code), "cmd") != LUA_OK
               || lua_pcall(L_, 0, LUA_MULTRET, 0) != LUA_OK;
    if (failed) {
        const char* err = lua_tostring(L_, -1);
        MD_LOG(MD_LOG_WARNING, "[LuaSystem] Exec error: %s", err ? err : "?");
        if (error_out && error_out_size) snprintf(error_out, error_out_size, "%s", err ? err : "?");
        lua_settop(L_, base);
        return false;
    }
    lua_settop(L_, base);
    return true;
}

bool LuaSystem::StartScenario(const char* lua_code, lua_Integer n_ticks,
                               char* error_out, size_t error_out_size) {
    if (!L_ || !lua_code) return false;
    scenario_co_ = lua_newthread(L_);  // GC-owned by L_; inherits the hook, see class doc comment
    scenario_started_ = false;
    if (luaL_loadstring(scenario_co_, lua_code) != LUA_OK) {
        const char* err = lua_tostring(scenario_co_, -1);
        if (error_out && error_out_size) snprintf(error_out, error_out_size, "%s", err ? err : "?");
        lua_pop(scenario_co_, 1);
        scenario_co_ = nullptr;
        return false;
    }
    lua_pushinteger(scenario_co_, n_ticks);
    return true;
}

LuaSystem::ScenarioResult LuaSystem::ResumeScenario() {
    ScenarioResult result;
    if (!scenario_co_) { snprintf(result.error_msg, sizeof(result.error_msg), "no active scenario"); return result; }

    // ALWAYS re-arm before resuming — see class doc comment / §Recon.7:
    // the instruction counter is cumulative, not fresh per resume.
    lua_sethook(scenario_co_, hook, LUA_MASKCOUNT, 50000);

    int nres = 0;
    bool first_call = !scenario_started_;
    scenario_started_ = true;
    int status = lua_resume(scenario_co_, L_, first_call ? 1 : 0, &nres);

    if (status == LUA_YIELD) {
        lua_pop(scenario_co_, nres);
        result.status = ScenarioStatus::Yielded;
        return result;
    }
    if (status == LUA_OK) {
        lua_pop(scenario_co_, nres);
        result.status = ScenarioStatus::Finished;
        scenario_co_ = nullptr;
        return result;
    }

    // Error: distinguish md.quit(code) (a table with __md_quit) from a
    // genuine failure (assert_*, wait_until timeout, runtime error).
    if (lua_istable(scenario_co_, -1)) {
        lua_getfield(scenario_co_, -1, "__md_quit");
        bool is_quit = lua_toboolean(scenario_co_, -1);
        lua_pop(scenario_co_, 1);
        if (is_quit) {
            lua_getfield(scenario_co_, -1, "code");
            result.quit_code = (int)lua_tointeger(scenario_co_, -1);
            lua_pop(scenario_co_, 2);  // code, then the error table itself
            result.status = ScenarioStatus::Quit;
            scenario_co_ = nullptr;
            return result;
        }
    }
    const char* err = lua_tostring(scenario_co_, -1);
    snprintf(result.error_msg, sizeof(result.error_msg), "%s", err ? err : "(non-string error)");
    lua_pop(scenario_co_, 1);
    result.status = ScenarioStatus::Failed;
    scenario_co_ = nullptr;
    return result;
}

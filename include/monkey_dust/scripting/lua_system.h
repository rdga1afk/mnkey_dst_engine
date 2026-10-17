#pragma once
#include <monkey_dust/ecs/md_entity.h>

extern "C" {
#ifdef _WIN32
#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
#else
#include <lua5.4/lua.h>
#include <lua5.4/lauxlib.h>
#include <lua5.4/lualib.h>
#endif
}

// LuaSystem — Lua 5.4 scripting host.
// Engine provides: Init/Shutdown, CallAction, RegisterFunction.
// Game registers its component-aware C functions via RegisterFunction().
// Sandbox guarantees:
//   - Instruction limit:   50 000 instructions/call (LUA_MASKCOUNT hook)
//   - Memory limit:        8 MB heap for the entire Lua state
//   - Blocked libraries:   io, os, package, debug, load, loadfile, dofile
static constexpr size_t LUA_MEM_LIMIT_BYTES = 8u * 1024u * 1024u; // 8 MB

class LuaSystem {
public:
    static LuaSystem& Get() {
        static LuaSystem inst;
        return inst;
    }

    bool Init(const char* scripts_dir);
    void Shutdown();

    // Let game register game-specific C API functions
    void RegisterFunction(const char* name, lua_CFunction fn);

    // Register a C function as a field of the global `md` table (creates
    // the table on first use). Autonomy-system functions (Etap 1,
    // AUTONOMY_SYSTEM_PROMPT_v2.md) live here, in the md.* namespace —
    // distinct from RegisterFunction's flat globals, which stay as the
    // pre-existing md_foo() convention (aliased into md.* at Init time,
    // not re-registered here).
    void RegisterNamespaceFunction(const char* field_name, lua_CFunction fn);

    // Register a C function as a field of a SUB-table under the global
    // `md` table (e.g. sub_table="editor", field_name="exec" registers
    // md.editor.exec). Creates both md and md[sub_table] on first use.
    // EDITOR_AUTOMATION_PLAN_v1.md Phase 2: the plan's API is written as
    // md.editor.*/md.ecs.* (real nested tables), not flat md.editor_foo()
    // like every existing binding (RegisterNamespaceFunction) — this is a
    // deliberate, separate registration path so existing flat bindings are
    // untouched; new Phase 2+ bindings use this one.
    void RegisterSubNamespaceFunction(const char* sub_table, const char* field_name, lua_CFunction fn);

    // Execute arbitrary Lua source; returns false on parse/runtime error.
    // Optional error_out captures the Lua error string (e.g. an
    // assert_true/assert_eq failure message) for callers that need to
    // report it back (editor command-file automation, task #123) rather
    // than only logging via MD_LOG_WARNING.
    bool Exec(const char* lua_code, char* error_out = nullptr, size_t error_out_size = 0);

    bool IsReady() const { return L_ != nullptr; }

    size_t MemUsed() const { return lua_mem_used_; }

    // ── Scenario coroutine driver (autonomy system, Etap 1) ─────────────
    // A scenario script's control flow (spawn -> wait_ticks -> assert ->
    // ...) runs inside ONE persistent Lua coroutine, driven by repeated
    // ResumeScenario() calls (one per logic tick) from the game loop.
    //
    // Empirically verified (docs/AUTONOMY_LOG.md §Recon.7, 4 standalone
    // probes): a lua_newthread() coroutine automatically inherits the
    // instruction-limit hook from the state that creates it — but the
    // instruction COUNTER is cumulative and never resets on its own, so a
    // long-running coroutine driven by many small resumes will eventually
    // exhaust whatever budget it inherited unless re-armed. ResumeScenario()
    // ALWAYS calls lua_sethook() on the coroutine's own lua_State* before
    // every lua_resume() — callers never need to (and must not) think
    // about this themselves.
    enum class ScenarioStatus { Yielded, Finished, Quit, Failed };
    struct ScenarioResult {
        ScenarioStatus status = ScenarioStatus::Failed;
        int  quit_code = 0;        // valid when status == Quit
        char error_msg[512] = {};  // valid when status == Failed
    };

    // Compiles `lua_code` as the scenario body (a coroutine) and pushes
    // `n_ticks` as its sole argument (accessible via `...` in the script).
    // Call once before the first ResumeScenario(). Returns false on parse
    // error (message copied into error_out).
    bool StartScenario(const char* lua_code, lua_Integer n_ticks,
                        char* error_out, size_t error_out_size);

    // Advances the scenario by one logic tick. Yielded = call again next
    // tick; Finished/Quit/Failed = scenario is done, driver should stop.
    ScenarioResult ResumeScenario();

private:
    LuaSystem() = default;

    lua_State* L_          = nullptr;
    lua_State* scenario_co_ = nullptr;  // owned by L_ (garbage-collected with it); not a separate alloc
    bool       scenario_started_ = false;
    size_t     lua_mem_used_ = 0;

    static void   hook(lua_State* L, lua_Debug* ar);
    static int    md_log(lua_State* L);
    static void*  lua_alloc(void* ud, void* ptr, size_t osize, size_t nsize);

    bool LoadFile(const char* path);
};

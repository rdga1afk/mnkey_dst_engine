#pragma once
#include <gaia.h>
#include <monkey_dust/ecs/gaia_sched_adapter.h>

// Глобальний singleton gaia::ecs::World (task #8 B3.4 — was entt::registry
// through B1-B3.3, flecs through the ECS-2 migration, gaia-ecs since
// 2026-09-10 — see CLAUDE_STATE.md for the full migration history).
// КРИТИЧНО: не передавати по значенню, не мутувати під
// час query ітерації. При потребі змінити entities під час ітерації —
// збирати в temp vector, застосовувати після завершення query.each().

// Registry::Get()'s real return type -- can't use `auto&` in a plain
// (non-template, C++17) function parameter, so any function signature that
// needs to hold "a world reference" generically (FlowGraph's action
// callbacks, BTSystem::Tick, etc.) uses this alias instead of hardcoding
// gaia::ecs::World&.
using MdWorldRef = gaia::ecs::World;

class Registry {
public:
    // Phase 4 (PROMPT_GAIA_MIGRATION.md §6): MdGaiaSchedAdapter::Install()
    // MUST run before the first possible parallel-exec query through this
    // World, else gaia lazily raises its own gaia::mt::ThreadPool on first
    // use (verified via probe, see gaia_sched_adapter.h's top comment) --
    // two thread pools on Intel HD 520 is unacceptable. Installed as part
    // of this static local's own initialization (guaranteed exactly-once,
    // before any caller can observe `w`), not a separate call site
    // someone could forget.
    // Defined in src/ecs/registry.cpp: an inline body made every TU that called
    // Get() instantiate and optimize World's ctor/dtor (#186, -ftime-trace).
    static gaia::ecs::World& Get();
    Registry() = delete;
};

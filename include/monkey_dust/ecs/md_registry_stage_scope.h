#pragma once
// md_registry_stage_scope.h — MdRegistryStageScope / MdManagedTag (split
// from md_registry.h, code audit proposal #12: docs/CODE_AUDIT_2026-09.md).

#include <gaia.h>

// Task #7/#8 concurrency project — real flecs-native multi-threading.
//
// Empirically verified (standalone probes, not guessed) before writing
// this: flecs query iteration (ecs_query_next, i.e. flecs::query::each())
// is NOT safe to run concurrently across 2 threads against the same
// flecs::world under ANY configuration EXCEPT world.readonly_begin(true)
// + each thread iterating a query that was BUILT ONCE against the main
// world BEFORE readonly_begin(), but ITERATED via query.iter(stage_ptr)
// where stage_ptr = world.get_stage(i) — "on-the-fly" query construction
// against a stage, or iterating via the raw (non-stage) world during
// readonly mode, both still crash (SIGSEGV in ecs_iter_fini). Separately
// verified: single-entity get_mut<T>()/try_get<T>() calls on the RAW
// (non-stage) world stay safe even while ANOTHER thread iterates a query
// via its stage — so ONLY query iteration needs stage-routing, not every
// MdRegistry call (confirmed via probe — see CLAUDE_STATE.md for the
// exact probe results).
//
// MdRegistryStageScope is the mechanism: a thread-local override that,
// when set, redirects MdView::each()/MdRegistry::Each() to iterate via
// query.iter(stage) instead of query.each() directly. Default (no scope
// active, the overwhelming majority of the game's execution) is
// byte-for-byte the same code path as before this feature existed — zero
// risk to anything outside the one JobGraph wave that sets it.
// Phase 1 stub: gaia has no stage/readonly-world concept analogous to
// flecs's world.get_stage(i) — the REAL replacement is the external
// scheduler adapter (w.set_sched(...) over the existing JobSystem),
// wired up in Phase 4 (prompt_/PROMPT_GAIA_MIGRATION.md §6). Until then
// this stores the override but MdEach/StagedHandle below do not act on
// it — every gaia-backend call runs against the raw world unconditionally,
// which is correct for Phase 1-3 (no code runs inside a JobGraph-staged
// batch on the gaia path yet, since JobGraph itself isn't ported until
// Phase 4). Flagged loudly, not silently: do NOT enable MD_ECS_GAIA
// together with any JobGraph-staged concurrent tick before Phase 4 lands.
namespace md_registry_detail {
    inline thread_local gaia::ecs::World* t_stage_override = nullptr;
}

class MdRegistryStageScope {
public:
    explicit MdRegistryStageScope(gaia::ecs::World& stage) noexcept
        : prev_(md_registry_detail::t_stage_override) {
        md_registry_detail::t_stage_override = &stage;
    }
    ~MdRegistryStageScope() { md_registry_detail::t_stage_override = prev_; }
    MdRegistryStageScope(const MdRegistryStageScope&) = delete;
    MdRegistryStageScope& operator=(const MdRegistryStageScope&) = delete;
private:
    gaia::ecs::World* prev_;
};

// MdManagedTag — task #8 B3.4. Every entity created via MdRegistry::Create()
// gets this tag, so MdRegistry::Each()/Clear()/Count() can scope "every
// entity I manage" without picking up flecs's own internal bootstrap/module
// entities — flecs's own world-wide entity iteration walks the ENTIRE
// entity index (hundreds of internal IDs for built-in components/modules),
// confirmed empirically; there's no other clean way to ask "just mine."
struct MdManagedTag {};

#pragma once
// md_registry_core.h — MdRegistry (split from md_registry.h, code audit
// proposal #12: docs/CODE_AUDIT_2026-09.md).

#include <monkey_dust/ecs/md_entity.h>
#include <monkey_dust/ecs/registry.h>
#include <monkey_dust/ecs/md_registry_stage_scope.h>
#include <monkey_dust/ecs/md_registry_query_helpers.h>
#include <monkey_dust/ecs/gaia_entity_handle.h>
#include <monkey_dust/platform/md_log.h>
#include <gaia.h>
#include <utility>

// MdRegistry — task #8 (EnTT->flecs strangler-fig migration), part B3.4.
//
// Thin facade over the SAME flecs::world singleton as Registry::Get() (was
// entt::registry through B1-B3.3) — MdRegistry::Get() wraps Registry::Get(),
// not a second world.
//
// Every method rehydrates a flecs::entity handle from the stored MdEntity's
// raw id via Handle() — flecs::entity handles are cheap (world pointer +
// id, no allocation, no query) so this per-call rehydration costs nothing
// meaningful. Raw() is the intentional escape hatch for code not yet
// retrofitted; View<T...>() caches its underlying query per unique T...
// signature (function-local static, added in B3.3 specifically to prepare
// for this: flecs::query construction registers with the world and is
// meant to be built once and reused, unlike entt::view's near-free
// construction).
//
// CRITICAL, B3.4: Handle(e).get_mut<T>() returns a T& that is only valid
// until the entity's NEXT archetype change — flecs relocates an entity's
// ENTIRE row to a new table on every emplace<U>/set<U>/remove<U> call, even
// for an unrelated component type U, invalidating every previously held T&
// for that entity (unlike entt's per-type-pool-stable references, where
// only the SAME type's pool reallocating could invalidate a ref). Rule:
// call Handle(e).emplace<T>()/set<T>() for every component an entity needs
// FIRST, THEN Handle(e).get_mut<T>() to fetch references for writing.
// get_mut<T>()/try_get_mut<T>() do not themselves invalidate anything (no
// structural change) — freely chaining several in a row is safe. Found and
// fixed 3 real production bugs of this exact shape (see CLAUDE_INVARIANTS.md).
class MdRegistry {
public:
    static MdRegistry& Get() {
        static MdRegistry inst;
        return inst;
    }

    // Task #52 (CLAUDE_STATE.md "Знахідка C", 2026-09-08): gaia reserves
    // entity ids [0..GAIA_ID_LastCoreComponent.id()] (currently 0..49 --
    // Core, EntityDesc, Component, ..., through the runtime-primitive-type
    // entities ending at F64, gaia.h:32132-32233) for its own bootstrap
    // state. Confirmed via a real crashing test
    // (DirectorSystemTest.BlackboardBroadcastOnTick) that gaia's own
    // entity-id allocator can, after enough create/delete churn across a
    // long-running World, hand w.add() one of these reserved ids back
    // WITHOUT bumping its generation -- making the "new" entity bit-
    // identical to (e.g.) gaia::ecs::Core and indistinguishable from it to
    // any query/has()/valid() check. Tagging that entity with
    // MdManagedTag corrupts gaia's own bootstrap state (MdRegistry::
    // Clear()'s later w.del() on it fails: "forbidden from being
    // deleted"), and any OTHER unrelated MdEach query that later matches
    // it can crash deep inside gaia's own each() machinery (confirmed:
    // BTSystem::Tick(), a plain non-Sparse query, segfaulted in
    // Chunk::comp_ptr_mut on exactly this entity).
    //
    // This is gaia's own allocator bug, not fixable from here -- but
    // every MdRegistry-managed entity is minted through this ONE
    // function, so refusing to ever hand out a reserved-range id to a
    // caller closes the actual hazard (a real user entity accidentally
    // aliasing gaia's bootstrap state) without needing to touch gaia's
    // vendored allocator internals. Bounded retry: this should be rare
    // (confirmed reachable only after substantial churn); a tight loop
    // here would indicate a much deeper allocator problem worth its own
    // investigation, not something to spin on silently.
    MdEntity Create() {
        auto& w = Raw();
        gaia::ecs::Entity e = w.add();
        int guard = 0;
        while (e.id() <= gaia::ecs::GAIA_ID_LastCoreComponent.id()) {
            MD_LOG(MD_LOG_WARNING,
                   "[MdRegistry::Create] w.add() returned a reserved gaia "
                   "core-namespace id (id=%u gen=%u, reserved range is "
                   "[0..%u]) -- discarding and retrying (task #52, see "
                   "CLAUDE_STATE.md Знахідка C).",
                   (unsigned)e.id(), (unsigned)e.gen(),
                   (unsigned)gaia::ecs::GAIA_ID_LastCoreComponent.id());
            w.del(e);  // best-effort; a no-op if e really is e.g. Core itself
            if (++guard >= 8) {
                MD_LOG(MD_LOG_ERROR,
                       "[MdRegistry::Create] w.add() kept returning "
                       "reserved-range ids after %d retries -- gaia's "
                       "allocator may be exhausted or more broadly broken "
                       "than task #52 anticipated. Proceeding with the "
                       "last id anyway to avoid an infinite loop.",
                       guard);
                break;
            }
            e = w.add();
        }
        w.add<MdManagedTag>(e);
        return MdEntity(e);
    }
    // See md_registry_detail's pre-destroy-hook doc comment above --
    // registers a callback invoked before an entity is actually removed
    // by Destroy() or Clear(), the only two entity-destruction call
    // sites in this facade. Idempotent by design: callers like
    // BTSystem::ConnectRegistry() run from per-TEST_F SetUp() across a
    // dozen+ fixtures in the same test binary, so the same function
    // pointer would otherwise be registered dozens of times and blow
    // through the fixed-size table.
    static void RegisterPreDestroyHook(md_registry_detail::PreDestroyHook fn) {
        for (size_t i = 0; i < md_registry_detail::g_pre_destroy_hook_count; ++i) {
            if (md_registry_detail::g_pre_destroy_hooks[i] == fn) return;
        }
        if (md_registry_detail::g_pre_destroy_hook_count < md_registry_detail::MAX_PRE_DESTROY_HOOKS) {
            md_registry_detail::g_pre_destroy_hooks[md_registry_detail::g_pre_destroy_hook_count++] = fn;
        } else {
            MD_LOG(MD_LOG_ERROR, "[MdRegistry] pre-destroy hook table full (MAX=%zu)",
                   md_registry_detail::MAX_PRE_DESTROY_HOOKS);
        }
    }

    void Destroy(MdEntity e) {
        md_registry_detail::RunPreDestroyHooks(Raw(), e.Raw());
        Handle(e).destruct();
    }
    bool Valid(MdEntity e) const {
        return e != MdEntity::Null() && Raw().valid(e.Raw());
    }

    template<typename T>
    void Remove(MdEntity e) { Handle(e).template remove<T>(); }

    template<typename T, typename... Args>
    T& Replace(MdEntity e, Args&&... args) {
        auto h = Handle(e);
        h.template set<T>() = T{std::forward<Args>(args)...};
        return h.template get_mut<T>();
    }

    template<typename T, typename Fn>
    void Patch(MdEntity e, Fn fn) {
        auto h = Handle(e);
        T& v = h.template get_mut<T>();
        fn(v);
        h.template modified<T>();
    }

    // See the flecs branch's Clear() comment for the full staging-hazard
    // rationale -- same logic applies; gaia's structural ops during a
    // scheduler-driven concurrent wave are the Phase 4 concern, not yet
    // wired up on this backend (see md_registry_detail::t_stage_override's
    // Phase-1-stub comment above).
    void Clear() {
        if (md_registry_detail::t_stage_override != nullptr) {
            MD_LOG(MD_LOG_ERROR,
                   "[MdRegistry] Clear() called while a JobGraph stage is "
                   "active — gaia backend does not route Clear() through "
                   "any stage yet (Phase 4). Move this call outside the "
                   "JobGraph batch. Clear() was NOT executed.");
            return;
        }
        // FOUND BY RUNTIME TEST: deleting entities while a query matching
        // them is still iterating aborts under gaia (fetch(): "Assertion
        // allowStaleExactPair || ... || allowStaleEntityRecord failed") --
        // the exact "mutate world during each()" hazard CLAUDE_CONSTITUTION.md
        // already names generically ("collect → apply патерн"), gaia just
        // enforces it with a hard assert where flecs's own version relied
        // on defer_begin/defer_end to make the in-place del() safe.
        // Phase 5 (prompt_/PROMPT_GAIA_MIGRATION.md §7 p.4): replaced the
        // earlier std::vector-collect+apply-after baseline with gaia's own
        // world-owned CommandBufferST -- del() calls made during iteration
        // are deferred and applied at commit(), gaia's own idiomatic
        // equivalent to flecs's defer_begin/defer_end (functionally
        // identical to the std::vector version this replaces; not
        // hot-path, real callers are editor-only per the flecs branch's
        // comment).
        // Task #52 (md_registry_detail::IsGaiaReservedEntity's doc
        // comment): confirmed via direct diagnostic that this exact query
        // (.all<MdManagedTag>()) intermittently yields gaia::ecs::Core as
        // a false-positive match after enough create/delete churn --
        // w.has(Core, MdManagedTag) checked false every time (37/37
        // occurrences), so it is not a real managed entity to delete.
        // MdEach's own entity-only dispatch branch (RestCount==0, which
        // this call uses) now filters any reserved-range entity out
        // before it ever reaches this lambda, so nothing reserved ever
        // reaches cmdBuf.del() below -- no per-entity check needed here.
        auto& w = Raw();
        auto& cmdBuf = w.cmd_buffer_st();
        auto q = w.query().all<MdManagedTag>();
        MdEach(q, [&](MdEntity e) {
            md_registry_detail::RunPreDestroyHooks(w, e.Raw());
            cmdBuf.del(e.Raw());
        });
        cmdBuf.commit();
    }

    // gaia's World::try_get(EntityId) IS the generation-aware alive-lookup
    // (verified probe P-F, docs/GAIA_MIGRATION_ANALYSIS.md UNKNOWN-1) --
    // simpler than flecs's ecs_get_alive path: it already returns
    // gaia::ecs::EntityBad on failure, no separate "dumb fallback" branch
    // needed the way FromIndex's flecs implementation requires.
    MdEntity FromIndex(uint32_t idx) const {
        gaia::ecs::Entity alive = Raw().try_get((gaia::ecs::EntityId)idx);
        return (alive != gaia::ecs::EntityBad) ? MdEntity(alive) : MdEntity(idx);
    }

    gaia::ecs::World& Raw() { return Registry::Get(); }
    const gaia::ecs::World& Raw() const { return Registry::Get(); }

    GaiaEntityHandle Handle(MdEntity e) const { return GaiaEntityHandle(const_cast<gaia::ecs::World&>(Raw()), e.Raw()); }

    // Phase-1 stub, matches MdRegistryStageScope's comment above: no real
    // stage/scheduler routing exists on the gaia backend yet (Phase 4),
    // so this always returns the same handle as Handle(). Kept as a
    // distinct call so Phase 4 has exactly one place to change.
    GaiaEntityHandle StagedHandle(MdEntity e) const { return Handle(e); }

    MdRegistry(const MdRegistry&) = delete;
    MdRegistry& operator=(const MdRegistry&) = delete;

private:
    MdRegistry() = default;
};

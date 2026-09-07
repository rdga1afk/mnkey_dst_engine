#pragma once
#include <monkey_dust/ecs/md_entity.h>
#include <monkey_dust/ecs/registry.h>
#include <monkey_dust/platform/md_log.h>
#if defined(MD_ECS_GAIA)
#include <gaia.h>
#else
#include <flecs.h>
#endif
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

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
#if defined(MD_ECS_GAIA)
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
#else
namespace md_registry_detail {
    inline thread_local ecs_world_t* t_stage_override = nullptr;
}

// RAII guard: while alive on the calling thread, MdView::each() iterates
// through `stage` instead of the raw world. Nest-safe (restores the prior
// value on destruction, not unconditionally nullptr) though nesting isn't
// expected in practice — one guard per JobGraph batch invocation.
class MdRegistryStageScope {
public:
    explicit MdRegistryStageScope(flecs::world& stage) noexcept
        : prev_(md_registry_detail::t_stage_override) {
        md_registry_detail::t_stage_override = stage.c_ptr();
    }
    ~MdRegistryStageScope() { md_registry_detail::t_stage_override = prev_; }
    MdRegistryStageScope(const MdRegistryStageScope&) = delete;
    MdRegistryStageScope& operator=(const MdRegistryStageScope&) = delete;
private:
    ecs_world_t* prev_;
};
#endif

// MdManagedTag — task #8 B3.4. Every entity created via MdRegistry::Create()
// gets this tag, so MdRegistry::Each()/Clear()/Count() can scope "every
// entity I manage" without picking up flecs's own internal bootstrap/module
// entities — flecs's own world-wide entity iteration walks the ENTIRE
// entity index (hundreds of internal IDs for built-in components/modules),
// confirmed empirically; there's no other clean way to ask "just mine."
struct MdManagedTag {};

// MdEach/MdFirst — task #8 Phase 3 (View<T...>() facade removal).
//
// Replace MdView, which hid a flecs::query<T...> behind a header-template
// function-local static — the SAME static, shared across every call site
// for a given T... pack WITHIN one linkage unit, but independently
// duplicated across a dlopen'd .so boundary (a real problem for the two
// hot-reloadable editor/gameplay .so targets: each side would silently
// get its own separate, separately-warmed query object for what looks
// like "the same" MdRegistry::View<T...>() call).
//
// The fix: callers now own an explicit flecs::query<T...> themselves —
// either a plain per-call-site `static auto q = reg.Raw().query<T...>();`
// for call sites with no cross-.so or cross-thread sharing need, or a
// named, non-template accessor function (defined once in a .cpp, e.g.
// game/src/ai/ai_queries.cpp) for the handful of signatures that DO need
// to be the same shared, pre-warmed object everywhere (the concurrent
// JobGraph wave's queries — see ai_queries.h for why). MdEach/MdFirst are
// stateless — safe to call on any flecs::query<T...>, regardless of who
// owns it or where it lives — so they can stay in this header without
// reintroducing the per-.so duplication problem (no static state here).
//
// Stage-awareness (t_stage_override) is preserved exactly as MdView had
// it: whichever query is passed in gets routed through iter(stage) when
// inside a JobGraph-staged batch, or iterated directly otherwise — this
// is what makes concurrent TickNeedsAndInjuries/TickAI query iteration
// safe (see the t_stage_override note above this file's opening comment).
//
// Callback signature: same convention as before — flecs::query::each()
// natively dispatches on whether Func accepts (flecs::entity, T&...) or
// just (T&...), so the wrapping lambda below only needs to convert that
// flecs::entity into MdEntity when the caller's Func wants an MdEntity
// first (checked the same way MdView did).
#if defined(MD_ECS_GAIA)
// gaia's query.each() ALWAYS delivers a chunk-level ecs::Iter&, never
// per-component args the way flecs's each() does (checked every each()
// example in gaia's README — Iter& or bare Entity, no exceptions found).
// This adapter bridges that real API-shape difference: it inspects
// Func's own parameter types via function_traits (extracted from
// operator(), same technique any generic-lambda-argument-deduction needs)
// and, per row in the chunk, calls func with the right combination of
// it.view<T>()/it.view_mut<T>() slices — verified against exactly this
// shape in /tmp/gaia_probe/pk_mdeach.cpp (both the (T1&,T2&...) and
// (MdEntity,T1&,T2&...) forms, including a real view_mut aliasing/mutation
// round-trip across two separate MdEach passes). Stage-routing is a
// documented Phase-1 no-op — see md_registry_detail::t_stage_override's
// comment above; real behavior lands with the Phase 4 scheduler adapter.
namespace md_registry_detail {
    template<typename T> struct function_traits : function_traits<decltype(&T::operator())> {};
    template<typename C, typename R, typename... Args>
    struct function_traits<R(C::*)(Args...) const> { using args_tuple = std::tuple<Args...>; };
    template<typename C, typename R, typename... Args>
    struct function_traits<R(C::*)(Args...)> { using args_tuple = std::tuple<Args...>; };

    template<typename Arg>
    decltype(auto) mdeach_view_for(gaia::ecs::Iter& it) {
        using Raw = std::remove_cv_t<std::remove_reference_t<Arg>>;
        if constexpr (std::is_const_v<std::remove_reference_t<Arg>>)
            return it.template view<Raw>();
        else
            return it.template view_mut<Raw>();
    }

    template<typename Func, typename ArgsTuple, size_t... I>
    void mdeach_dispatch_row(Func& func, gaia::ecs::Iter& it, uint32_t row,
                              std::index_sequence<I...>) {
        func(mdeach_view_for<std::tuple_element_t<I, ArgsTuple>>(it)[row]...);
    }

    template<typename Func, typename ArgsTuple, size_t... I>
    void mdeach_dispatch_row_with_entity(Func& func, gaia::ecs::Iter& it, uint32_t row,
                                          gaia::ecs::Entity e, std::index_sequence<I...>) {
        func(MdEntity(e), mdeach_view_for<std::tuple_element_t<I + 1, ArgsTuple>>(it)[row]...);
    }
}

template<typename Query, typename Func>
void MdEach(Query& q, Func&& func) {
    using Traits = md_registry_detail::function_traits<std::decay_t<Func>>;
    using ArgsTuple = typename Traits::args_tuple;
    constexpr size_t N = std::tuple_size_v<ArgsTuple>;
    constexpr bool wantsEntity = N > 0 &&
        std::is_same_v<std::tuple_element_t<0, ArgsTuple>, MdEntity>;

    q.each([&](gaia::ecs::Iter& it) {
        auto entView = it.template view<gaia::ecs::Entity>();
        if constexpr (wantsEntity) {
            for (uint32_t row = 0; row < it.size(); ++row)
                md_registry_detail::mdeach_dispatch_row_with_entity<Func, ArgsTuple>(
                    func, it, row, entView[row], std::make_index_sequence<N - 1>{});
        } else {
            for (uint32_t row = 0; row < it.size(); ++row)
                md_registry_detail::mdeach_dispatch_row<Func, ArgsTuple>(
                    func, it, row, std::make_index_sequence<N>{});
        }
    });
}

// gaia's Query is a single non-templated type (using Query =
// detail::QueryImpl) -- unlike flecs::query<Components...>, it carries no
// compile-time component list, so MdFirst cannot be templated on
// Components... here. No query-level early-exit primitive was found
// (verified: no find()-equivalent in gaia's public Query API) -- this
// scans the full query and keeps the first match, which is correct but
// not a true early-exit like flecs::query::find(). Acceptable for Phase 1
// (MdFirst is a rare call, not the ~91-site MdEach hot path); revisit if
// profiling later shows otherwise.
inline MdEntity MdFirst(gaia::ecs::Query& q) {
    gaia::ecs::Entity found = gaia::ecs::EntityBad;
    q.each([&](gaia::ecs::Iter& it) {
        if (found != gaia::ecs::EntityBad || it.size() == 0)
            return;
        found = it.template view<gaia::ecs::Entity>()[0];
    });
    return (found != gaia::ecs::EntityBad) ? MdEntity(found) : MdEntity::Null();
}
#else
template<typename Query, typename Func>
void MdEach(Query& q, Func&& func) {
    ecs_world_t* stage = md_registry_detail::t_stage_override;
    auto wrapped = [&func](flecs::entity fe, auto&... args) {
        if constexpr (std::is_invocable_v<Func, MdEntity, decltype(args)&...>) {
            func(MdEntity(fe.id()), args...);
        } else {
            func(args...);
        }
    };
    if (stage) q.iter(stage).each(wrapped);
    else       q.each(wrapped);
}

// First matching entity, or MdEntity::Null() if none — a real early-exit
// via flecs::query::find() (unlike MdView::front(), which had to visit
// every match since flecs's each() has no early-exit protocol). Templated
// on the query's Components... pack (not a generic `auto&...` predicate)
// because flecs::find_delegate resolves the predicate against TWO
// candidate signatures — (flecs::iter&, size_t, Components&...) and plain
// (Components&...) — and a fully generic lambda matches both at once,
// making overload resolution ambiguous.
template<typename... Components>
MdEntity MdFirst(flecs::query<Components...>& q) {
    flecs::entity found = q.find([](Components&...) { return true; });
    return found ? MdEntity(found.id()) : MdEntity::Null();
}
#endif

#if defined(MD_ECS_GAIA)
// GaiaEntityHandle — Phase 1 replacement for MdRegistry::Handle()'s return
// type. flecs::entity is a real library type carrying (world, id) that
// exposes get_mut<T>/has<T>/etc AS ITS OWN methods; gaia has no equivalent
// object — its API shape is World::method<T>(entity), not
// entity.method<T>(). This proxy reproduces exactly the 10 methods
// actually used at the ~870 Handle(e).X<T>() call sites
// (docs/GAIA_SEAM_AUDIT.md §1.1), each mapped and verified against the
// v1.0.0 tag source directly, not by symmetry with flecs:
//   try_get_mut<T>() -- world.h has NO nullable component accessor
//     (mut()/get()/sset() all say "Undefined behavior otherwise" on
//     absence, world.h:5745/5782/6062) -- composed has<T>()+mut<T>() is
//     the only replacement, TWO lookups instead of one. This is the
//     hottest facade path (436 sites, 198 in bt_vm_ext.inc alone) --
//     GATE 1's mandatory microbenchmark (prompt_/PROMPT_GAIA_MIGRATION.md
//     §3 p.4) targets exactly this method.
//   try_get<T>(target) -- pair form of the same gap (npc_relationship.h,
//     2 sites). Same has()+get() composition, via a pair-encoded Entity.
//   modified<T>() -- gaia's real equivalent is modify<T, true>(entity)
//     (world.h:5591, doc comment: "Triggers set side effects if true" /
//     "set hooks and OnSet observers") -- NOT a no-op, verified name and
//     semantics from source, not assumed absent.
class GaiaEntityHandle {
public:
    GaiaEntityHandle(gaia::ecs::World& w, gaia::ecs::Entity e) : w_(w), e_(e) {}

    template<typename T>
    T& get_mut() { return w_.template mut<T>(e_); }

    // GATE 1 microbenchmark result (/tmp/gaia_probe/bench/, 500 entities,
    // real project headers, -O3 -DNDEBUG matched on both sides -- an
    // earlier run without -O3 on flecs.c gave a false "gaia is faster"
    // result, caught before being trusted): the naive has<T>()+mut<T>()
    // composition (two independent resolves) measured ~42ns present /
    // ~82ns absent vs flecs's native ~13ns / ~12ns -- 3-7x slower, well
    // outside GATE 1's 1.5x budget. mut_raw(entity, component)
    // (world.h:5842) does the SAME resolve gaia's own has()+mut() would
    // do internally, but ONCE instead of twice -- exactly the
    // "reuse EntityContainer/chunk-lookup" direction
    // prompt_/PROMPT_GAIA_MIGRATION.md §3 p.4 asks to investigate before
    // accepting the naive version. Cut it to ~25ns present / ~23ns absent
    // (~2x flecs, not 3-7x) -- still over budget but the honest number,
    // not a guess. mut_raw is also a SILENT write (doc comment: "call
    // World::modify_raw(...) after writing through data directly") --
    // this matches, not weakens, the existing contract: flecs's own
    // try_get_mut()/get_mut() do not auto-trigger change notification
    // either (this file's class comment above, "do not themselves
    // invalidate anything"); MdRegistry::Patch<T>() is the call site that
    // explicitly opts into notification via modified<T>().
    template<typename T>
    T* try_get_mut() {
        auto compEntity = w_.template add<T>().entity;
        auto view = w_.mut_raw(e_, compEntity);
        return view.valid() ? reinterpret_cast<T*>(view.data) : nullptr;
    }

    template<typename T>
    const T* try_get(gaia::ecs::Entity target) const {
        auto relEntity = w_.template add<T>().entity;
        auto pairEnt = (gaia::ecs::Entity)gaia::ecs::Pair(relEntity, target);
        return w_.has(e_, pairEnt) ? &w_.template get<T>(e_, pairEnt) : nullptr;
    }

    // Pair-payload set (relation type T, target entity, value) -- verified
    // against v1.0.0 directly, not by symmetry with the 0-arg set() above:
    // add<T>(entity, pairEnt, value) (world.h:3754) is a genuine safe
    // upsert for pairs, confirmed by re-calling it on an ALREADY-existing
    // pair in /tmp/gaia_probe/pc_upd_tag.cpp ("after repeat add: 99",
    // overwrites correctly) -- no separate has+create branch needed here,
    // unlike the non-pair set() above which genuinely needs one.
    template<typename T>
    void set(gaia::ecs::Entity target, T value) {
        auto relEntity = w_.template add<T>().entity;
        auto pairEnt = (gaia::ecs::Entity)gaia::ecs::Pair(relEntity, target);
        w_.template add<T>(e_, pairEnt, std::move(value));
    }

    // Pair removal -- w.del(entity, pairEntity) verified in the same probe
    // (pc_upd_tag.cpp: "has after del: 0"), but ONLY when the pair already
    // exists. FOUND BY RUNTIME TEST
    // (/tmp/gaia_probe/test_npc_relationship_gaia_runtime.cpp): calling
    // del() on a pair that was never added asserts
    // (m_world.valid(entity)) inside gaia's own EntityBuilder::del --
    // NpcRelationshipComponent::ClearAll() calls remove<Fear>(other) for
    // every tracked entity unconditionally, including ones that only ever
    // had Trust set, never Fear. flecs's remove<T>(pair) is a safe no-op
    // on absence; gaia's del() is not -- same presence-contract mismatch
    // as the 0-arg remove()/set() family, has-guard is the fix here too.
    template<typename T>
    void remove(gaia::ecs::Entity target) {
        auto relEntity = w_.template add<T>().entity;
        auto pairEnt = (gaia::ecs::Entity)gaia::ecs::Pair(relEntity, target);
        if (w_.has(e_, pairEnt))
            w_.del(e_, pairEnt);
    }

    template<typename T>
    bool has() const { return w_.template has<T>(e_); }

    // gaia's set<T>(entity) is a write-back proxy (README "Change
    // detection"), not a 2-arg call -- Handle(e).set<T>() = value; keeps
    // working unchanged at call sites via this same proxy-return pattern.
    // FOUND BY RUNTIME TEST, NOT DOCS (/tmp/gaia_probe/test_md_registry_
    // gaia_runtime.cpp): unlike flecs's set<T>(value), gaia's set<T>(entity)
    // is NOT a safe upsert -- world.h:5707 says "It is expected the
    // component is present on entity. Undefined behavior otherwise,"
    // because the proxy copies the CURRENT value via an internal get<T>()
    // to seed the write-back. Calling it on an entity that never had T
    // aborts (GAIA_ASSERT owner != EntityBad inside get<T>()) instead of
    // adding T the way flecs's set() would. has<T>()+add<T>() guard
    // restores the upsert contract MdRegistry::Replace<T>()'s own comment
    // already documents ("flecs's set() is a safe upsert either way").
    // Same root-cause family as try_get_mut<T>/try_get(pair): gaia expects
    // presence-checking at the call site, not baked into the convenience
    // method.
    template<typename T>
    decltype(auto) set() {
        if (!w_.template has<T>(e_))
            w_.template add<T>(e_, T{});
        return w_.template set<T>(e_);
    }

    // 2-arg convenience form matching flecs::entity::set<T>(value) exactly
    // (found by real build failure, not by re-reading the call-site audit:
    // offscreen_npc_db.cpp:217 calls Handle(e).set<T>(value) directly,
    // not the 0-arg proxy form). Same has+add upsert guard as the 0-arg
    // overload above -- same root-cause family, same fix.
    template<typename T>
    void set(T value) {
        if (!w_.template has<T>(e_))
            w_.template add<T>(e_, T{});
        w_.template set<T>(e_) = std::move(value);
    }

    template<typename T, typename... Args>
    void emplace(Args&&... args) {
        w_.template add<T>(e_, T{std::forward<Args>(args)...});
    }

    // has-guard is required here too -- del<T>(entity) has the SAME
    // "expected present, undefined behavior otherwise" contract as the
    // set<T>() family above (world.h del<T>'s own doc comment says so
    // explicitly), unlike flecs's remove<T>() which is a safe no-op on an
    // absent component. FOUND BY RUNTIME TEST
    // (/tmp/gaia_probe/test_npc_relationship_gaia_runtime.cpp,
    // NpcRelationshipComponent::ClearAll calling remove<Fear> on an entity
    // that only ever had Trust set) -- third instance of this exact
    // presence-contract mismatch class this session (try_get_mut, set()).
    template<typename T>
    void remove() { if (w_.template has<T>(e_)) w_.template del<T>(e_); }

    template<typename T>
    void modified() { w_.template modify<T, true>(e_); }

    bool is_alive() const { return w_.valid(e_); }
    void destruct() { w_.del(e_); }

private:
    gaia::ecs::World& w_;
    gaia::ecs::Entity e_;
};
#endif

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
#if defined(MD_ECS_GAIA)
class MdRegistry {
public:
    static MdRegistry& Get() {
        static MdRegistry inst;
        return inst;
    }

    MdEntity Create() {
        auto e = Raw().add();
        Raw().add<MdManagedTag>(e);
        return MdEntity(e);
    }
    void Destroy(MdEntity e) { Handle(e).destruct(); }
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
        // on defer_begin/defer_end to make the in-place del() safe. Collect
        // first, delete after iteration completes. Not hot-path (real
        // callers are editor-only per the flecs branch's comment), so
        // std::vector here does not violate the hot-path fixed-array rule.
        // Phase 5 (prompt_/PROMPT_GAIA_MIGRATION.md §7 p.4) may replace
        // this with ecs::CommandBufferST for a closer idiomatic match to
        // flecs's defer_begin/defer_end -- functionally equivalent either
        // way, this is the verified-correct baseline.
        auto& w = Raw();
        auto q = w.query().all<MdManagedTag>();
        std::vector<gaia::ecs::Entity> toDelete;
        MdEach(q, [&](MdEntity e) { toDelete.push_back(e.Raw()); });
        for (gaia::ecs::Entity e : toDelete) w.del(e);
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
#else
class MdRegistry {
public:
    static MdRegistry& Get() {
        static MdRegistry inst;
        return inst;
    }

    MdEntity Create() {
        auto e = Raw().entity();
        e.add<MdManagedTag>();
        return MdEntity(e.id());
    }
    void Destroy(MdEntity e) { Handle(e).destruct(); }
    // flecs's ecs_is_alive() requires entity != 0 (ecs_check, aborts in
    // debug builds otherwise) — MdEntity::Null() is id_=0, so it must be
    // rejected here before ever reaching is_alive().
    bool Valid(MdEntity e) const {
        return e != MdEntity::Null() && Handle(e).is_alive();
    }

    template<typename T>
    void Remove(MdEntity e) { Handle(e).template remove<T>(); }

    // entt's replace() requires T already present; flecs's set() is a safe
    // upsert either way (verified: does not crash/assert if T is absent),
    // so this is slightly more permissive than the old entt contract but
    // not unsafe.
    template<typename T, typename... Args>
    T& Replace(MdEntity e, Args&&... args) {
        auto h = Handle(e);
        h.template set<T>(T{std::forward<Args>(args)...});
        return h.get_mut<T>();
    }

    template<typename T, typename Fn>
    void Patch(MdEntity e, Fn fn) {
        auto h = Handle(e);
        T& v = h.get_mut<T>();
        fn(v);
        h.template modified<T>();
    }

    // Destroys every MdRegistry-managed entity (tagged MdManagedTag) —
    // does NOT touch flecs's own internal bootstrap/module entities.
    // defer_begin/defer_end: destructing an entity mid-.each() mutates its
    // own table (locked for the duration of iteration) — flecs's internal
    // ecs_assert(!table->_->lock) catches this in debug builds (SIGABRT);
    // in release (NDEBUG) the assert compiles away and the mutation still
    // happens, silently violating "no reg mutation during view.each()".
    // Deferring queues the destructs until after iteration completes.
    // Point 1 (concurrency audit): Clear() is NOT stage-aware (builds a
    // fresh, non-static query and calls .each() directly, bypassing
    // MdEach()'s t_stage_override check) — see job_graph.h's caveat. No
    // current call site reaches Clear() from inside a JobGraph batch (both
    // real callers — editor_toolbar.cpp, scene_serializer.h — are
    // editor-only, main-thread, outside JobGraph::Run()'s window), so this
    // is a latent-risk guard, not a fix for an active bug: it turns future
    // silent UB into a loud signal instead. Deliberately NOT gated behind
    // #ifndef NDEBUG: CLAUDE.md's own documented default build is
    // `-DCMAKE_BUILD_TYPE=Release` (confirmed via build/CMakeCache.txt), so
    // an NDEBUG-gated check would never fire in the configuration this
    // project actually builds/tests with day to day. The check itself is a
    // single pointer compare — negligible next to Clear()'s own query+.each()
    // cost — so there is no real reason to gate it at all.
    //
    // Early-return (not just log-and-continue): Clear()'s body calls
    // defer_begin()/query().each()/defer_end() on the RAW (non-stage) world
    // — running that while a JobGraph wave's readonly_begin(true) is active
    // is exactly the "structural op during readonly mode" hazard flecs's own
    // FLECS_SANITIZE checks (Debug builds, engine/CMakeLists.txt) assert on.
    // Confirmed by running this guard's own test under
    // -DCMAKE_BUILD_TYPE=Debug -DMD_SANITIZE=asan (Phase 4.4 audit): logging
    // alone still let Clear() run its unsafe body and abort — skipping the
    // body once the hazard is detected is the actual fix, not just a louder
    // warning.
    void Clear() {
        if (md_registry_detail::t_stage_override != nullptr) {
            MD_LOG(MD_LOG_ERROR,
                   "[MdRegistry] Clear() called while a JobGraph stage is "
                   "active — Clear()'s query+.each() does not route through "
                   "t_stage_override and will race with concurrent staged "
                   "query iteration. Move this call outside the JobGraph "
                   "batch. Clear() was NOT executed.");
            return;
        }
        auto& w = Raw();
        w.defer_begin();
        w.query<MdManagedTag>().each([](flecs::entity e, MdManagedTag) { e.destruct(); });
        w.defer_end();
    }

    // Reconstruct an MdEntity from a stored uint32 index (e.g.
    // BlackboardEntry::val.e, Lua integer args) — resolves to the
    // CURRENTLY alive entity for that index via flecs's generation-aware
    // lookup, safe against the index having been recycled by a different,
    // later-created entity since the id was stored. Falls back to a
    // generation-0 MdEntity (same as MdEntity(uint32_t) directly) if no
    // alive entity currently holds that index.
    MdEntity FromIndex(uint32_t idx) const {
        ecs_entity_t alive = ecs_get_alive(Raw().c_ptr(), (ecs_entity_t)idx);
        return alive ? MdEntity(alive) : MdEntity(idx);
    }

    flecs::world& Raw() { return Registry::Get(); }
    const flecs::world& Raw() const { return Registry::Get(); }

    // Native flecs::entity handle for MdEntity e — the facade-removal escape
    // hatch (task #8 phase-out): call sites migrating off Emplace/GetOrEmplace/
    // EmplaceOrReplace use Handle(e).emplace<T>()/.set<T>() directly instead
    // of the old T&-returning facade methods, since flecs::entity::emplace/set
    // return the entity itself (not T&), which is what makes the B3.4
    // dangling-reference bug class structurally impossible here.
    flecs::entity Handle(MdEntity e) const { return flecs::entity(Raw(), e.Raw()); }

    // task #8 Phase 5: stage-routed handle for STRUCTURAL ops (emplace/
    // set/destruct/remove) issued from code that might run inside a
    // JobGraph-staged batch (see MdRegistryStageScope above). During
    // world.readonly_begin(true) (which JobGraph::Run() wraps its wave
    // in), flecs forbids structural ops on the main world outright — "readonly
    // assert" — but permits them on a STAGE, where they're automatically
    // queued and merged back into the world on readonly_end() (readonly_
    // begin/end are documented to internally bracket defer_begin/defer_end
    // — see flecs.h's ecs_readonly_begin() doc comment). This is what
    // replaces DeferredStructuralOps' 3 hand-rolled queues: route the
    // structural call through StagedHandle() instead of Handle(), and
    // flecs's own deferred-command mechanism does the rest, generalizing
    // to any future structural op instead of only the 3 that were
    // manually audited and queued before.
    //
    // Falls back to the plain (main-world) Handle() when no stage is
    // active (t_stage_override unset) — i.e. this is always safe to call,
    // inside or outside a JobGraph batch, unlike the old Queue*() calls
    // which only made sense because Flush() ran them through the real
    // structural path afterward.
    //
    // Read-only accessors (Get/TryGet/AllOf/has) are NOT routed through
    // this — the existing get_mut<T>()/try_get<T>() on the RAW world
    // during a concurrent stage's iteration was separately verified safe
    // by probe (see md_registry.h's top-of-file note); only structural
    // ops need the stage.
    //
    // A second, easy-to-miss distinction (empirically verified, standalone
    // flecs probes — red/green-tested against game/src/combat/skill_xp.h's
    // real lost-update bug, see tests/behavior/test_skill_xp_staged_grant.
    // cpp): StagedHandle().set<T>() is genuinely DEFERRED — invisible even
    // through the SAME stage's own reads — ONLY the first time T is added to
    // an entity (a real structural/archetype change). Once T already exists
    // on the entity, a stage-routed set<>() to it is a plain VALUE write and
    // applies immediately, visible to the very next read (staged or main-
    // world) in the same batch — flecs does not defer it at all, because no
    // archetype move is needed. Practical implication for any code called
    // from inside a JobGraph batch: reading a MAIN-world (Handle()) snapshot
    // of T, computing on it, and writing the WHOLE struct back via a single
    // StagedHandle().set<T>() is safe to repeat multiple times per entity
    // per batch ONLY if T already existed on that entity BEFORE the batch
    // started. If T might be getting its first-ever add mid-batch, that
    // pattern silently loses every write but the last one for that entity —
    // prefer MdRegistry::Patch<T>() (in-place mutation via a main-world
    // pointer) once T is known to exist, and treat "T doesn't exist yet" as
    // its own explicit, single, no-computation StagedHandle().set<T>(T{})
    // branch rather than folding it into the same read-compute-write path.
    flecs::entity StagedHandle(MdEntity e) const {
        ecs_world_t* stage = md_registry_detail::t_stage_override;
        return stage ? flecs::entity(stage, e.Raw()) : Handle(e);
    }

    MdRegistry(const MdRegistry&) = delete;
    MdRegistry& operator=(const MdRegistry&) = delete;

private:
    MdRegistry() = default;
};
#endif

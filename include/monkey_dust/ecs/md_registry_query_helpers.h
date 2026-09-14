#pragma once
// md_registry_query_helpers.h — MdEach / MdFirst + their supporting
// md_registry_detail template machinery (split from md_registry.h, code
// audit proposal #12: docs/CODE_AUDIT_2026-09.md). This is the dense
// SFINAE-dispatch block working around several gaia-ecs library bugs
// (Sparse-storage view crash, Entity+Sparse compile failure, reserved
// bootstrap-entity false-positive query matches) -- extracted verbatim,
// byte-exact via `sed` line-range extraction, not retyped, given how
// easily a transcription slip could silently change dispatch behavior
// here without a compile error to catch it.

#include <monkey_dust/ecs/md_entity.h>
#include <monkey_dust/platform/md_log.h>
#include <gaia.h>
#include <tuple>
#include <type_traits>
#include <utility>

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
// MdEach routes through gaia's NATIVE typed .each<Components...>(func)
// overload (SFINAE-selected when Func does NOT take Iter&, gaia.h:61073),
// not the Iter&-callback each() this used to call. Found 2026-09-08
// (task #47/#49): the Iter&-callback path (it.view<T>()/it.view_mut<T>(),
// eventually Chunk::comp_ptr_mut) assumes every component has chunk-column
// storage -- for a GAIA_STORAGE(Sparse) component there is no such column
// (payload lives in a separate entity-indexed store), so it.view<T>()/
// view_mut<T>() null-derefs and crashes (gdb-confirmed: null rec.pData,
// DirectorSystemTest.BlackboardBroadcastOnTick and others). gaia's typed
// each() dispatches Sparse correctly via run_query_on_chunks_sparse_typed/
// run_query_on_sparse_entities_typed (QueryPlanMode::SparseDense,
// gaia.h:57052/58514/58519/60060/63123-63348) -- real entity-indexed
// per-row accessors (TypedSparseQueryView/typed_sparse_chunk_view,
// gaia.h:62942-62978) the Iter&-callback path never used at all. Audited
// all 91 real MdEach call sites before this rewrite: none use Iter&/chunk
// access, so none needed the old path's extra capability, and Entity-as-
// argument is natively supported by the typed path too (gaia.h:62995,
// `if constexpr (std::is_same_v<U, Entity>) return entity;` -- works at
// any argument index, not just first). Stage-routing is a documented
// Phase-1 no-op — see md_registry_detail::t_stage_override's comment
// above; Phase 4 (job_graph.cpp) settled on no stage/readonly_begin
// equivalent being needed for gaia at all.
namespace md_registry_detail {
    // Task #52 (CLAUDE_STATE.md "Знахідка C", 2026-09-08): gaia reserves
    // entity ids [0..GAIA_ID_LastCoreComponent.id()] (currently 0..49 --
    // Core, EntityDesc, Component, ..., through the runtime-primitive-type
    // entities ending at F64, gaia.h:32132-32233) for its own bootstrap
    // state. Every one of these is explicitly given .add(Pair(OnDelete,
    // Error)) and .add(Core) at bootstrap (gaia.h:80828-80950) -- the
    // doc comment on Core itself says "the entity it is attached to is
    // ignored by queries" (gaia.h:32131), so bootstrap entities are
    // SUPPOSED to be invisible to ordinary user queries.
    //
    // Confirmed by direct diagnostic (not assumed) that this guarantee
    // does not always hold: MdRegistry::Clear()'s query
    // (.all<MdManagedTag>()) periodically yields gaia::ecs::Core
    // (id=0,gen=0) as a match, after enough create/delete churn across
    // the full ~340-file test binary. w.has(Core, MdManagedTag) was
    // checked directly at the point of the false match and returned
    // FALSE every time (37/37 occurrences logged) -- so this is a real
    // query-result correctness bug (almost certainly in one of
    // QueryInfo's several layered result caches, gaia.h:50705 onward --
    // seedArchetypeCache/archetypeCache/directChunks, each with its own
    // world-version-keyed invalidation), NOT entity-id-allocator
    // corruption (an earlier, now-falsified hypothesis: guarding
    // MdRegistry::Create() against handing out a reserved id to a real
    // caller had zero effect -- the warning never fired, yet the false
    // match persisted identically). Root-causing the exact missing cache
    // invalidation trigger inside gaia's query engine was not attempted
    // beyond this point -- multiple interacting version counters across
    // several cache layers make it a substantial, separate investigation
    // safely done only with much more time than reproducing this find
    // already took (~10 failed standalone-probe attempts, and it never
    // reproduced in isolation -- only in the accumulated-churn full
    // suite).
    //
    // Given (a) w.has() is the one thing PROVEN reliable here, and (b)
    // this codebase's OWN entity allocator (MdRegistry::Create(), see its
    // doc comment) never hands a real managed entity an id in this
    // range, ANY entity with id() in [0..GAIA_ID_LastCoreComponent] that
    // reaches user code through a query is, by construction in this
    // codebase, always a leaked gaia bootstrap identifier -- filtering
    // strictly by id RANGE (not by re-querying has() against every term,
    // which MdEach's generic dispatch has no cheap way to enumerate) is
    // the safe, general, low-risk guard every dispatch path below applies.
    GAIA_NODISCARD inline bool IsGaiaReservedEntity(gaia::ecs::Entity e) {
        return e.id() <= gaia::ecs::GAIA_ID_LastCoreComponent.id();
    }

    // Pre-destroy hooks — BT-leak investigation (CLAUDE_STATE.md БОРГ entry).
    //
    // gaia::ecs::ObserverEvent::OnDel only fires when a component leaves a
    // STILL-LIVING entity's archetype (a component-removal transition) --
    // confirmed via standalone repro that it structurally never fires for
    // whole-entity destruction (w.del(entity)), unlike flecs's equivalent.
    // A real non-trivial C++ destructor on the component type was also
    // tried and rejected: gaia's chunk shift/move helpers (gaia.h,
    // shift_elements_left/right_aos, move_elements_aos) move-ASSIGN
    // non-trivial types during chunk compaction/growth rather than
    // construct+destroy, which a standalone stress repro (50 owning
    // entities under storage churn) showed drops roughly half of the
    // real frees and can double-free a copied component — worse than the
    // leak it would fix.
    //
    // The only two call sites that ever destroy an MdRegistry-managed
    // entity are MdRegistry::Destroy() and MdRegistry::Clear() below --
    // both invoke every registered hook immediately before the entity is
    // actually removed, so this is the one place guaranteed to fire for
    // every destruction path (single-entity and bulk). Fixed-size table,
    // no malloc (CLAUDE.md hot-path rule) -- registration happens a
    // handful of times at startup (BTSystem::ConnectRegistry() etc.), not
    // per-entity.
    using PreDestroyHook = void(*)(gaia::ecs::World&, gaia::ecs::Entity);
    inline constexpr size_t MAX_PRE_DESTROY_HOOKS = 8;
    inline PreDestroyHook g_pre_destroy_hooks[MAX_PRE_DESTROY_HOOKS] = {};
    inline size_t g_pre_destroy_hook_count = 0;

    inline void RunPreDestroyHooks(gaia::ecs::World& w, gaia::ecs::Entity e) {
        for (size_t i = 0; i < g_pre_destroy_hook_count; ++i)
            g_pre_destroy_hooks[i](w, e);
    }

    template<typename T> struct function_traits : function_traits<decltype(&T::operator())> {};
    template<typename C, typename R, typename... Args>
    struct function_traits<R(C::*)(Args...) const> { using args_tuple = std::tuple<Args...>; };
    template<typename C, typename R, typename... Args>
    struct function_traits<R(C::*)(Args...)> { using args_tuple = std::tuple<Args...>; };

    // gaia's chunk-column accessors (sview_auto -> sview_mut/view)
    // static_assert(!is_empty_v<U>) and refuse to fetch ANY empty type's
    // "value" at all, Sparse or not, Table or not -- a genuinely
    // zero-size tag (e.g. MdManagedTag) has no backing storage for gaia
    // to hand out a reference to, and the typed dispatch path has no
    // Sparse-style special case for it either (found 2026-09-08 rewriting
    // MdEach for Sparse; a separate, pre-existing gaia limitation this
    // works around, not something the Sparse fix itself touches). Worked
    // around by never asking gaia for such a term's value at all: gaia's
    // typed each() only requires a callback's requested args be a SUBSET
    // of the query's declared terms (has_all<T...>, gaia.h:62076 --
    // not an exact match), so omitting an empty-type arg from what the
    // wrapper asks for is safe -- entity matching still comes from the
    // query's own .all<T&>() declaration. AllEmpty below is only ever
    // true for a bare tag term (every real call site today has zero
    // non-empty args alongside one, per an exhaustive grep) -- AnyEmpty
    // catches the untested "mixed" shape explicitly rather than silently
    // doing something unverified.
    template<typename ArgsTuple, size_t Offset, size_t... I>
    constexpr bool AnyEmpty(std::index_sequence<I...>) {
        return (std::is_empty_v<std::remove_cv_t<std::remove_reference_t<
            std::tuple_element_t<Offset + I, ArgsTuple>>>> || ...);
    }
    template<typename ArgsTuple, size_t Offset, size_t... I>
    constexpr bool AllEmpty(std::index_sequence<I...>) {
        return (std::is_empty_v<std::remove_cv_t<std::remove_reference_t<
            std::tuple_element_t<Offset + I, ArgsTuple>>>> && ...);
    }

    template<typename Func, typename ArgsTuple, size_t Offset, size_t... I>
    void CallAllEmpty(Func& func, std::index_sequence<I...>) {
        func(std::tuple_element_t<Offset + I, ArgsTuple>{}...);
    }
    template<typename Func, typename ArgsTuple, size_t Offset, size_t... I>
    void CallAllEmptyWithEntity(Func& func, MdEntity e, std::index_sequence<I...>) {
        func(e, std::tuple_element_t<Offset + I, ArgsTuple>{}...);
    }

    // User callbacks take MdEntity (this codebase's own entity wrapper),
    // never gaia::ecs::Entity directly -- gaia's typed each() needs to see
    // a signature it recognizes (Entity or a registered component type),
    // so this wrapper stands between: gaia calls it with (Entity, T1&...),
    // it converts Entity -> MdEntity and forwards to the real func. Held
    // by reference, not copied -- the wrapper itself is passed BY VALUE
    // into gaia's each(Func func), which only copies the reference member,
    // not whatever func captures.
    template<typename Func, typename ArgsTuple, size_t... I>
    struct TypedEntityWrapper {
        Func& func;
        void operator()(gaia::ecs::Entity e, std::tuple_element_t<I + 1, ArgsTuple>... args) const {
            // Task #52 guard -- see IsGaiaReservedEntity's doc comment.
            if (IsGaiaReservedEntity(e)) {
                MD_LOG(MD_LOG_WARNING,
                       "[MdEach] typed dispatch got a reserved gaia "
                       "bootstrap entity (id=%u gen=%u) as a query match "
                       "-- skipping (task #52).",
                       (unsigned)e.id(), (unsigned)e.gen());
                return;
            }
            func(MdEntity(e), args...);
        }
    };
    template<typename Func, typename ArgsTuple, size_t... I>
    TypedEntityWrapper<Func, ArgsTuple, I...> MakeTypedEntityWrapper(Func& func, std::index_sequence<I...>) {
        return TypedEntityWrapper<Func, ArgsTuple, I...>{func};
    }

    // Is T Sparse-storage under gaia? Reuses gaia's own storage-policy
    // detection rather than hardcoding the 5 component names -- stays
    // correct automatically if more components become
    // GAIA_STORAGE(Sparse) later.
    template<typename T>
    inline constexpr bool IsSparseV =
        gaia::ecs::auto_storage_policy_v<T> == gaia::ecs::DataStorageType::Sparse;

    template<typename ArgsTuple, size_t Offset, size_t... I>
    constexpr bool AnySparse(std::index_sequence<I...>) {
        return (IsSparseV<std::remove_cv_t<std::remove_reference_t<
            std::tuple_element_t<Offset + I, ArgsTuple>>>> || ...);
    }

    // gaia's typed each() has a SEPARATE bug from the Sparse view crash
    // this file's TypedEntityWrapper works around above (task #51,
    // 2026-09-08): asking it for Entity AND a Sparse-storage component
    // TOGETHER fails to COMPILE.
    // world_query_entity_arg_by_id<Entity>() (gaia.h:85298-85325) has
    // inconsistent decltype(auto) return-type deduction -- an early
    // `if constexpr (is_same_v<Arg,Entity>) return entity;` with no
    // `else`, followed by more code that is UNCONDITIONALLY instantiated
    // regardless of that branch (if constexpr without else doesn't
    // exclude subsequent statements from instantiation, only from
    // execution) and returns a DIFFERENT type (const Arg&, i.e.
    // const Entity& for Arg=Entity). Reachable whenever Entity appears in
    // a typed each() callback's arg list alongside ANY Sparse-storage
    // type: each_typed_inter's Sparse branches are selected by a plain
    // runtime `if (plan.mode == ...)`, not `if constexpr`, so BOTH
    // candidate query-plan-mode functions get instantiated regardless of
    // which one a given query shape would actually use at runtime -- no
    // way to dodge this by restructuring the query. Worked around by
    // never asking gaia's typed each() for Entity when a Sparse arg is
    // also present: falls back to the Iter&-callback path for
    // iteration/entity (both safe -- Entity is never Sparse-storage), and
    // for each Sparse-storage arg specifically, reads/writes it via
    // mut_raw() per-row instead of it.view<T>()/view_mut<T>() (which
    // crashes on Sparse, per this file's top doc comment). mut_raw() has
    // its own explicit, correct Sparse branch (gaia.h:71133,
    // DataStorageType::Sparse check) -- an already-proven primitive
    // (GaiaEntityHandle::try_get_mut<T> above, bt_system.cpp's own
    // GaiaTryGetMut). compEntity resolved fresh per call, not cached in a
    // static -- MdEach may run against a test-local World, not just the
    // global Registry::Get() singleton (same reasoning as
    // bt_system.cpp's GaiaTryGetMut, which this mirrors).
    template<typename T>
    T* HybridMutRaw(gaia::ecs::World& w, gaia::ecs::Entity e) {
        auto compEntity = w.template add<T>().entity;
        auto view = w.mut_raw(e, compEntity);
        return view.valid() ? reinterpret_cast<T*>(view.data) : nullptr;
    }

    template<typename Arg>
    decltype(auto) HybridArgFor(gaia::ecs::World& w, gaia::ecs::Iter& it, uint32_t row, gaia::ecs::Entity e) {
        using Raw = std::remove_cv_t<std::remove_reference_t<Arg>>;
        if constexpr (IsSparseV<Raw>) {
            // Used for both const and mutable Arg: mut_raw() is a
            // silent-write path (no auto side effects just from being
            // called, per its own doc comment) so there's no behavioral
            // difference for a read-only caller, matching this facade's
            // existing try_get_mut<T>() convention of not distinguishing
            // the two at this level.
            //
            // GAIA_ASSERT here compiles to nothing under NDEBUG (this
            // project's actual Release build) -- verified by disassembly
            // during task #49's investigation: a null p silently became a
            // null-pointer dereference several frames later instead of an
            // assert failure. HybridDispatchRowWithEntity's w.valid(e)
            // check above is the real guard now; this stays a genuine
            // (non-empty-in-Release) check as defense in depth, since
            // w.valid(e) confirms the ENTITY is alive but not that this
            // SPECIFIC sparse component is attached to it.
            Raw* p = HybridMutRaw<Raw>(w, e);
            if (p == nullptr) {
                static Raw s_dummy{};
                MD_LOG(MD_LOG_WARNING,
                       "[MdEach] hybrid dispatch: entity (id=%u gen=%u) is "
                       "alive but missing the Sparse component the query "
                       "declared present -- returning a dummy value instead "
                       "of dereferencing null. See HybridArgFor's doc "
                       "comment (task #49).",
                       (unsigned)e.id(), (unsigned)e.gen());
                // Parenthesized: decltype(auto) on a bare identifier
                // deduces the declared type (Raw, by value); on a
                // parenthesized id-expression it deduces Raw& instead,
                // matching `return *p;` below -- the exact "inconsistent
                // deduction for auto return type" trap task #51's doc
                // comment above describes gaia itself falling into.
                return (s_dummy);
            }
            return *p;
        } else if constexpr (std::is_const_v<std::remove_reference_t<Arg>>) {
            return it.template view<Raw>()[row];
        } else {
            return it.template view_mut<Raw>()[row];
        }
    }

    // 2026-09-08 runtime finding (debug-instrumented reproduction, task
    // #49): gaia's Iter&-callback dispatch for a query whose ONLY term is a
    // Sparse-storage component (e.g. .all<AgentBlackboard&>()) can hand
    // back a row whose gaia::ecs::Entity is a bogus {id=0,gen=0} that does
    // NOT correspond to any live entity -- confirmed via a real crashing
    // test (DirectorSystemTest.BlackboardBroadcastOnTick): it.size()==1,
    // the chunk's own entity_view() row is {0,0}, yet the real npc entity
    // (created moments earlier) has an entirely different, valid id.
    // Reproducing this in an isolated standalone probe (same component
    // shapes/sizes, same Create-then-emplace-then-emplace structural
    // sequence, chunk recycling, multi-entity chunks, component
    // pre-warmup) was NOT possible -- the defect appears to depend on
    // accumulated world state this project's 340+ test binary produces
    // that a minimal repro can't cheaply reconstruct. Rather than guess
    // further at gaia's internals, this is guarded defensively at the one
    // point that actually matters: never trust an Entity read back from
    // this dispatch path without confirming it against the world's own
    // alive-check first. A mismatch is logged (loud, not silent) and the
    // row is skipped instead of dereferencing a possibly-null pointer.
    template<typename Func, typename ArgsTuple, size_t Offset, size_t... I>
    void HybridDispatchRowWithEntity(Func& func, gaia::ecs::World& w, gaia::ecs::Iter& it, uint32_t row,
                                      gaia::ecs::Entity e, std::index_sequence<I...>) {
        // Task #52 (IsGaiaReservedEntity's doc comment): checked FIRST and
        // ahead of w.valid() below -- w.valid() is not reliable evidence
        // here, since gaia::ecs::Core itself is a legitimately "alive"
        // bootstrap entity (w.valid(Core) returns true); the id-range
        // check is what's actually proven to catch this case.
        if (IsGaiaReservedEntity(e)) {
            MD_LOG(MD_LOG_WARNING,
                   "[MdEach] hybrid dispatch got a reserved gaia bootstrap "
                   "entity (id=%u gen=%u) from a Sparse-only query row -- "
                   "skipping (task #52).",
                   (unsigned)e.id(), (unsigned)e.gen());
            return;
        }
        if (!w.valid(e)) {
            MD_LOG(MD_LOG_WARNING,
                   "[MdEach] hybrid dispatch got an invalid entity (id=%u "
                   "gen=%u) from a Sparse-only query row -- skipping. See "
                   "HybridDispatchRowWithEntity's doc comment (task #49).",
                   (unsigned)e.id(), (unsigned)e.gen());
            return;
        }
        func(MdEntity(e), HybridArgFor<std::tuple_element_t<Offset + I, ArgsTuple>>(w, it, row, e)...);
    }
}

template<typename Query, typename Func>
void MdEach(Query& q, Func&& func) {
    using Traits = md_registry_detail::function_traits<std::decay_t<Func>>;
    using ArgsTuple = typename Traits::args_tuple;
    constexpr size_t N = std::tuple_size_v<ArgsTuple>;
    constexpr bool wantsEntity = N > 0 &&
        std::is_same_v<std::tuple_element_t<0, ArgsTuple>, MdEntity>;
    constexpr size_t Offset = wantsEntity ? 1 : 0;
    constexpr size_t RestCount = N - Offset;

    if constexpr (RestCount == 0) {
        // Entity-only callback -- nothing to fetch at all.
        q.each([&](gaia::ecs::Entity e) {
            // Task #52 guard -- see IsGaiaReservedEntity's doc comment.
            // MdRegistry::Clear() is this branch's most consequential
            // caller (its query is Entity-only) -- confirmed via direct
            // diagnostic that .all<MdManagedTag>() intermittently yields
            // gaia::ecs::Core here.
            if (md_registry_detail::IsGaiaReservedEntity(e)) {
                MD_LOG(MD_LOG_WARNING,
                       "[MdEach] entity-only dispatch got a reserved gaia "
                       "bootstrap entity (id=%u gen=%u) as a query match "
                       "-- skipping (task #52).",
                       (unsigned)e.id(), (unsigned)e.gen());
                return;
            }
            func(MdEntity(e));
        });
    } else if constexpr (md_registry_detail::AllEmpty<ArgsTuple, Offset>(std::make_index_sequence<RestCount>{})) {
        if constexpr (wantsEntity) {
            q.each([&](gaia::ecs::Entity e) {
                // Task #52 guard -- see IsGaiaReservedEntity's doc comment.
                if (md_registry_detail::IsGaiaReservedEntity(e)) {
                    MD_LOG(MD_LOG_WARNING,
                           "[MdEach] all-empty dispatch got a reserved "
                           "gaia bootstrap entity (id=%u gen=%u) as a "
                           "query match -- skipping (task #52).",
                           (unsigned)e.id(), (unsigned)e.gen());
                    return;
                }
                md_registry_detail::CallAllEmptyWithEntity<Func, ArgsTuple, Offset>(
                    func, MdEntity(e), std::make_index_sequence<RestCount>{});
            });
        } else {
            q.each([&]() {
                md_registry_detail::CallAllEmpty<Func, ArgsTuple, Offset>(
                    func, std::make_index_sequence<RestCount>{});
            });
        }
    } else if constexpr (md_registry_detail::AnyEmpty<ArgsTuple, Offset>(std::make_index_sequence<RestCount>{})) {
        static_assert(!md_registry_detail::AnyEmpty<ArgsTuple, Offset>(std::make_index_sequence<RestCount>{}),
            "MdEach: mixing an empty/zero-size component (e.g. a tag type) "
            "with a real component in the same callback is not supported "
            "under gaia (no real call site needs this today, verified "
            "2026-09-08) -- split into two MdEach calls, or extend this "
            "file's AllEmpty-only handling to a general filter if this "
            "combination becomes genuinely needed.");
    } else if constexpr (wantsEntity &&
                          md_registry_detail::AnySparse<ArgsTuple, Offset>(std::make_index_sequence<RestCount>{})) {
        // gaia's typed each() can't combine Entity + a Sparse component
        // in one callback (task #51) -- hybrid Iter&-callback path
        // instead, see HybridArgFor's doc comment above for why.
        // QueryImpl has no public world() of its own (m_storage is
        // private) -- fetch() (the same call .each() itself makes first,
        // "creates or refreshes the backing QueryInfo if needed") returns
        // QueryInfo&, which does expose a public world().
        gaia::ecs::World& w = *q.fetch().world();
        q.each([&](gaia::ecs::Iter& it) {
            auto entView = it.template view<gaia::ecs::Entity>();
            for (uint32_t row = 0; row < it.size(); ++row)
                md_registry_detail::HybridDispatchRowWithEntity<Func, ArgsTuple, Offset>(
                    func, w, it, row, entView[row], std::make_index_sequence<RestCount>{});
        });
    } else if constexpr (wantsEntity) {
        auto wrapper = md_registry_detail::MakeTypedEntityWrapper<Func, ArgsTuple>(
            func, std::make_index_sequence<RestCount>{});
        q.each(wrapper);
    } else {
        // No MdEntity conversion needed -- func's own signature already
        // matches what gaia's typed each() expects component-wise, so it
        // can be called directly with zero wrapper overhead. (A Sparse
        // arg here, without Entity, doesn't hit task #51's bug -- that's
        // specifically about combining Entity with Sparse.)
        //
        // Known gap (task #52): this is the one MdEach branch that can't
        // apply IsGaiaReservedEntity's guard -- func's signature carries
        // no Entity at all here, so there is nothing to check without
        // wrapping every call (the exact per-call overhead this branch
        // exists to avoid). Not yet observed to hit the reserved-entity
        // false-positive in practice (every confirmed occurrence so far
        // was in an entity-taking branch), but structurally exposed the
        // same way if it ever does.
        q.each(std::forward<Func>(func));
    }
}

// gaia's Query is a single non-templated type (using Query =
// detail::QueryImpl) -- unlike flecs::query<Components...>, it carries no
// compile-time component list, so MdFirst cannot be templated on
// Components... here, and can't route through MdEach's typed-each() path
// above (which needs the callback's own parameter types to identify what
// to fetch -- MdFirst's signature carries none). Stays on the untyped
// Iter&-callback path deliberately: it never calls it.view<T>()/
// view_mut<T>() for any query-declared component (only it.size() and
// it.view<Entity>(), and Entity is never Sparse-storage or empty), so it
// doesn't hit MdEach's task #51 compile-time bug. No query-level
// early-exit primitive was found (verified: no find()-equivalent in
// gaia's public Query API) -- this scans the full query and keeps the
// first match, which is correct but not a true early-exit like
// flecs::query::find(). Acceptable for Phase 1 (MdFirst is a rare call,
// not the ~91-site MdEach hot path); revisit if profiling later shows
// otherwise.
//
// Guards added 2026-09-08 (tasks #49, #52): the SAME Iter&-callback +
// it.view<Entity>() mechanism used here was proven (via
// DirectorSystemTest.BlackboardBroadcastOnTick, BTSystem::Tick()) to be
// able to return gaia's own reserved bootstrap entities (e.g.
// gaia::ecs::Core, id=0,gen=0) as a false-positive query match -- see
// md_registry_detail::IsGaiaReservedEntity's doc comment for the full
// finding (this is a query-result-cache correctness bug in gaia, not
// entity-allocator corruption -- confirmed via w.has() at the point of
// the false match). All current MdFirst call sites use at least one
// Table-storage term (StatSheet-only, or PlayerController+WorldTransform
// mixed) and haven't shown this failure mode in practice, but the guard
// is cheap and prevents this from becoming a live crash the day it does.
inline MdEntity MdFirst(gaia::ecs::Query& q) {
    gaia::ecs::Entity found = gaia::ecs::EntityBad;
    q.each([&](gaia::ecs::Iter& it) {
        if (found != gaia::ecs::EntityBad || it.size() == 0)
            return;
        gaia::ecs::Entity candidate = it.template view<gaia::ecs::Entity>()[0];
        if (md_registry_detail::IsGaiaReservedEntity(candidate)) {
            MD_LOG(MD_LOG_WARNING,
                   "[MdFirst] query matched a reserved gaia bootstrap "
                   "entity (id=%u gen=%u) -- skipping (task #52).",
                   (unsigned)candidate.id(), (unsigned)candidate.gen());
            return;
        }
        found = candidate;
    });
    return (found != gaia::ecs::EntityBad) ? MdEntity(found) : MdEntity::Null();
}

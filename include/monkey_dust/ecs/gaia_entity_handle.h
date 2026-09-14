#pragma once
// gaia_entity_handle.h — GaiaEntityHandle (split from md_registry.h, code
// audit proposal #12: docs/CODE_AUDIT_2026-09.md).

#include <gaia.h>
#include <utility>

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
    // Second optimization pass, per the same §3 p.4 requirement (tried
    // BEFORE settling, not guessed): caching the component entity in a
    // per-T function-local static (safe -- w_ always refers to the same
    // Registry::Get() singleton World regardless of which GaiaEntityHandle
    // instance triggers first-touch init) shaves the redundant add<T>()
    // component-cache lookup on every call. Measured improvement was
    // modest, not transformative (~25ns present roughly unchanged, ~23ns
    // absent down to ~21ns) -- add<T>() itself was already cheap; the
    // remaining ~20-25ns is mut_raw()'s own internal resolve
    // (id_owner_inter + component_item), which appears to be gaia's real,
    // architectural per-call floor for this operation. Final ratio vs
    // flecs: ~2.2x present, ~1.8x absent -- both still over GATE 1's 1.5x
    // budget after two independent, measured optimization attempts.
    template<typename T>
    T* try_get_mut() {
        static const gaia::ecs::Entity compEntity = w_.template add<T>().entity;
        auto view = w_.mut_raw(e_, compEntity);
        return view.valid() ? reinterpret_cast<T*>(view.data) : nullptr;
    }

    template<typename T>
    const T* try_get(gaia::ecs::Entity target) const {
        static const gaia::ecs::Entity relEntity = w_.template add<T>().entity;
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
        static const gaia::ecs::Entity relEntity = w_.template add<T>().entity;
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
        static const gaia::ecs::Entity relEntity = w_.template add<T>().entity;
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

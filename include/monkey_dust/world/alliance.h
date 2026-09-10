#pragma once
#include <cstdint>
#include <gaia.h>

// ── AllianceGroup ─────────────────────────────────────────────────────────────
// MD ALLIANCE_GROUP — faction membership categories.
enum class AllianceGroup : uint8_t {
    Unaligned    = 0,
    Player       = 1,
    Alien        = 2,
    Android      = 3,
    Seegson      = 4,
    WeylandYutani= 5,
    Hostile      = 6,
    Neutral      = 7,
    Friendly     = 8,
};

// ── AllianceStance ────────────────────────────────────────────────────────────
enum class AllianceStance : uint8_t {
    Friendly = 0,
    Neutral  = 1,
    Hostile  = 2,
};

// ── AllianceMatrix ────────────────────────────────────────────────────────────
// Singleton stance table over the 9 AllianceGroup values. Default: Hostile
// between (Player,Alien), (Player,Android), (Alien,Seegson),
// (Alien,WeylandYutani); else Neutral.
//
// Task #8 B4 pilot: backed by relation pairs (HostileWith/FriendlyWith) on
// 9 private per-group entities instead of a 9×9 array — absence of either
// pair means Neutral (the default), so only non-Neutral stances cost
// storage. Public API (GetStance/SetStance/IsEnemy) is unchanged; every
// caller in the codebase only ever goes through it, never touched internals
// directly, so this is a pure implementation swap.
// Uses its OWN private World, not MdRegistry's global one — alliance
// groups are a fixed enum-indexed lookup table, not gameplay entities, and
// keeping them isolated avoids any interaction with MdRegistry::Clear().
//
// gaia-ecs port (2026-09-10, flecs removal): bare tag-pair add/has/del —
// gaia.h:69045 World::add(Entity,Entity), :71518 has(Entity,Entity),
// :70784 del(Entity,Entity) — take the pair's already-composed Entity
// (gaia::ecs::Pair(relEntity, target) cast to Entity), matching the exact
// shape GaiaEntityHandle::try_get<T>() (md_registry.h) already verified
// against v1.0.0 directly for the payload-pair case; this is the same
// composition without a value.
class AllianceMatrix {
public:
    static AllianceMatrix& Get() noexcept {
        static AllianceMatrix inst;
        return inst;
    }

    AllianceStance GetStance(AllianceGroup a, AllianceGroup b) const noexcept {
        gaia::ecs::Entity ea = group_[idx(a)];
        gaia::ecs::Entity eb = group_[idx(b)];
        auto hostilePair  = (gaia::ecs::Entity)gaia::ecs::Pair(hostileWith_, eb);
        auto friendlyPair = (gaia::ecs::Entity)gaia::ecs::Pair(friendlyWith_, eb);
        if (world_.has(ea, hostilePair))  return AllianceStance::Hostile;
        if (world_.has(ea, friendlyPair)) return AllianceStance::Friendly;
        return AllianceStance::Neutral;
    }

    void SetStance(AllianceGroup a, AllianceGroup b, AllianceStance s) noexcept {
        gaia::ecs::Entity ea = group_[idx(a)];
        gaia::ecs::Entity eb = group_[idx(b)];
        auto hostileAB  = (gaia::ecs::Entity)gaia::ecs::Pair(hostileWith_, eb);
        auto hostileBA  = (gaia::ecs::Entity)gaia::ecs::Pair(hostileWith_, ea);
        auto friendlyAB = (gaia::ecs::Entity)gaia::ecs::Pair(friendlyWith_, eb);
        auto friendlyBA = (gaia::ecs::Entity)gaia::ecs::Pair(friendlyWith_, ea);
        world_.del(ea, hostileAB);  world_.del(eb, hostileBA);
        world_.del(ea, friendlyAB); world_.del(eb, friendlyBA);
        if (s == AllianceStance::Hostile)  { world_.add(ea, hostileAB);  world_.add(eb, hostileBA); }
        if (s == AllianceStance::Friendly) { world_.add(ea, friendlyAB); world_.add(eb, friendlyBA); }
    }

    bool IsEnemy(AllianceGroup a, AllianceGroup b) const noexcept {
        return GetStance(a, b) == AllianceStance::Hostile;
    }

private:
    static constexpr uint8_t N = 9;

    static uint8_t idx(AllianceGroup g) noexcept { return static_cast<uint8_t>(g); }

    AllianceMatrix() noexcept {
        // Relation tags — zero-size, never instantiated as component data;
        // registering them once here (add<T>().entity) gives the stable
        // Entity id gaia::ecs::Pair() composes against below.
        hostileWith_  = world_.add<HostileWith>().entity;
        friendlyWith_ = world_.add<FriendlyWith>().entity;
        for (uint8_t i = 0; i < N; ++i) group_[i] = world_.add();

        auto hostile = [&](AllianceGroup a, AllianceGroup b) { SetStance(a, b, AllianceStance::Hostile); };
        hostile(AllianceGroup::Player, AllianceGroup::Alien);
        hostile(AllianceGroup::Player, AllianceGroup::Android);
        hostile(AllianceGroup::Alien,  AllianceGroup::Seegson);
        hostile(AllianceGroup::Alien,  AllianceGroup::WeylandYutani);
        hostile(AllianceGroup::Hostile, AllianceGroup::Player);
        hostile(AllianceGroup::Hostile, AllianceGroup::Friendly);

        auto friendly = [&](AllianceGroup a, AllianceGroup b) { SetStance(a, b, AllianceStance::Friendly); };
        friendly(AllianceGroup::Player,  AllianceGroup::Friendly);
        friendly(AllianceGroup::Seegson, AllianceGroup::WeylandYutani);
    }

    // Relation tags — zero-size, never instantiated as component data.
    struct HostileWith  {};
    struct FriendlyWith {};

    gaia::ecs::World  world_;
    gaia::ecs::Entity group_[N];
    gaia::ecs::Entity hostileWith_;
    gaia::ecs::Entity friendlyWith_;
};

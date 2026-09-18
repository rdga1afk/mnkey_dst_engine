#pragma once
#include <cstdint>
#include <monkey_dust/ecs/md_entity.h>
#include <monkey_dust/ecs/md_registry.h>
#include <monkey_dust/platform/md_log.h>
#include <gaia.h>

// ── NpcRelationshipComponent ──────────────────────────────────────────────────
// Echo-engine inspired inter-NPC relationship system.
// Tracks bilateral trust/fear values between specific NPC pairs.
// Orthogonal to AllianceMatrix (faction-level) — this is per-entity.
//
// trust: 0=hostile, 128=neutral, 255=fully trusted
// fear:  0=no fear, 255=maximum fear (triggers Flee motivation)
//
// Task #8 B4.2: trust/fear values are stored as flecs (Trust,other)/
// (Fear,other) relation pairs directly on the owning entity — NOT inside
// this struct. This struct now holds ONLY the fixed-8 FIFO eviction order
// (recent[]) needed to bound flecs's otherwise-unbounded pair storage to
// the same MAX_RELATIONS=8 guarantee the old array-based design had.
//
// Every method now takes `self` explicitly (the owning entity) since flecs
// relation pairs are inherently entity-relative — there's no way to
// set/query a pair without a real entity to hang it on. The old design let
// NpcRelationshipComponent work as a bare value type with no entity at all
// (unit-tested that way pre-B4.2); that contract is gone by design — every
// call site now needs a real entity (reg.Create()), which is a genuine
// public-API change, not just an implementation swap behind a stable
// facade the way AllianceMatrix (B4.1) was.
struct NpcRelationshipComponent {
    static constexpr uint8_t MAX_RELATIONS = 8;

    MdEntity recent[MAX_RELATIONS] = {};
    uint8_t  count                 = 0;
    uint8_t  _pad[7]               = {};

    void SetTrust(MdEntity self, MdEntity other, uint8_t val) noexcept {
        Track(self, other);
        Handle(self).set<Trust>(other.Raw(), Trust{val});
    }
    void SetFear(MdEntity self, MdEntity other, uint8_t val) noexcept {
        Track(self, other);
        Handle(self).set<Fear>(other.Raw(), Fear{val});
    }

    // Returns 128 (neutral) when no relation exists.
    uint8_t GetTrust(MdEntity self, MdEntity other) const noexcept {
        const Trust* t = Handle(self).try_get<Trust>(other.Raw());
        return t ? t->value : 128u;
    }
    // Returns 0 (no fear) when no relation exists.
    uint8_t GetFear(MdEntity self, MdEntity other) const noexcept {
        const Fear* f = Handle(self).try_get<Fear>(other.Raw());
        return f ? f->value : 0u;
    }

    void ClearAll(MdEntity self) noexcept {
        auto h = Handle(self);
        for (uint8_t i = 0; i < count; ++i) {
            h.remove<Trust>(recent[i].Raw());
            h.remove<Fear>(recent[i].Raw());
        }
        count = 0;
    }

private:
    // Relation types WITH payload — a flecs pair where the relation itself
    // carries data (self.set<Trust>(other, {val})), not a zero-size tag.
    struct Trust { uint8_t value = 128; };
    struct Fear  { uint8_t value = 0;   };

    // GaiaEntityHandle's pair-aware set<T>(target,value)/try_get<T>(target)/
    // remove<T>(target) (md_registry.h) implement exactly this file's needs
    // -- reused directly rather than re-deriving the ecs::Pair(relEntity,
    // target) composition here a second time.
    //
    // ECS_CLOSEOUT.md §0.3: was `GaiaEntityHandle(Registry::Get(), e.Raw())`
    // -- a literal duplicate of MdRegistry::Get().Handle(e) that happened to
    // bypass the facade instead of calling it. Same object either way
    // (MdRegistry::Raw() IS Registry::Get()); routing through the facade is
    // the only externally-visible change.
    static GaiaEntityHandle Handle(MdEntity e) noexcept {
        return MdRegistry::Get().Handle(e);
    }

    // Fixed-8 FIFO eviction bookkeeping — bounds flecs's otherwise-
    // unbounded pair storage to the same guarantee the old array had. Not
    // a true LRU (evicts oldest-INSERTED, not oldest-ACCESSED) — matches
    // the old FindOrCreate's exact eviction order bit for bit.
    void Track(MdEntity self, MdEntity other) noexcept {
        for (uint8_t i = 0; i < count; ++i)
            if (recent[i] == other) return;  // already tracked
        if (count < MAX_RELATIONS) {
            recent[count++] = other;
            return;
        }
        MD_LOG(MD_LOG_WARNING, "NpcRelationshipComponent: full, evicting slot 0");
        auto h = Handle(self);
        h.remove<Trust>(recent[0].Raw());
        h.remove<Fear>(recent[0].Raw());
        for (uint8_t i = 0; i < MAX_RELATIONS - 1; ++i) recent[i] = recent[i + 1];
        recent[MAX_RELATIONS - 1] = other;
    }
};
// B4.2: recent[8] (64B, MdEntity=8B) + count/pad (8B) = 72B — smaller than
// the old 136B since trust/fear values moved out into flecs pair storage.
static_assert(sizeof(NpcRelationshipComponent) == 72, "NpcRelationshipComponent must be 72 bytes");

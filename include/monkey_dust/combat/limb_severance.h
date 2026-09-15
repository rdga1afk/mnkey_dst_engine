#pragma once
#include <monkey_dust/ecs/md_entity.h>
#include <monkey_dust/ecs/md_registry.h>
#include <monkey_dust/components/health.h>
#include <monkey_dust/combat/damage_calc.h>

// LimbSeverance — Kenshi-style limb detachment on KO with Cut damage.
//
// Trigger: damage type = Cut, effective damage ≥ SEVER_THRESHOLD,
//          limb hp transitions to ≤ 0 for the first time (cripple → sever).
// Effect:  LimbHealth::Sever(limb) on source entity; spawn a detached
//          prop entity at the limb's world position (render handle only —
//          no physics until RagdollSystem is integrated).
//
// Prosthetics: Equip(entity, limb_idx, prosthetic_max_hp) calls
//              LimbHealth::AttachProsthetic and clears SEVERED flag.

static constexpr float SEVER_THRESHOLD_CUT = 35.f; // min effective damage to sever a limb

// Attempt to sever a limb on entity `e` if conditions are met.
// Returns true if the limb was severed this call.
// reg    — ECS registry (must not be iterated when called)
// e      — entity taking damage
// limb   — 0-5 limb index (LIMB_COUNT)
// damage — effective damage (post-armour)
// type   — must be DamageType::Cut to allow severance
// hit_dir — normalised world-space direction of the hit (for detach velocity)
bool LimbSeverance_TryApply(MdRegistry& reg, MdEntity e,
                             int limb, float damage, DamageType type,
                             const float hit_dir[3]);

// Equip a prosthetic on a severed limb.
// prosthetic_max_hp — HP granted by the prosthetic.
// Returns false if limb is not severed or index out of range.
bool LimbSeverance_Equip(MdRegistry& reg, MdEntity e,
                          int limb, float prosthetic_max_hp);

// Detached limb entity — spawned by LimbSeverance_TryApply.
// Has WorldTransform; rendered as a static prop (no physics body for now).
struct DetachedLimb {
    MdEntity source;    // entity that lost the limb
    int          limb_idx;  // 0-5
    float        velocity[3]; // initial detach impulse direction
};

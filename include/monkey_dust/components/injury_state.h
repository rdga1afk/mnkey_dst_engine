#pragma once
// InjuryState — per-limb bleeding, resting heal, and KO recovery.
// Extends BleedComponent (single-limb active wound) with per-limb tracking.
// BleedSystem reads InjuryState when present; falls back to BleedComponent otherwise.
//
// Kenshi RE:
//   blood recovery rate   = HP/s while being treated by a doctor
//   bleeding clot rate    = HP/s natural clotting without treatment
//   resting heal rate mult= multiplier when resting on floor vs in a bed
//   stun recovery rate    = seconds until KO ends naturally

#include <monkey_dust/components/health.h>  // LIMB_COUNT
#include <cstdint>

static constexpr float BLEED_CLOT_RATE        = 0.003f;  // HP/s natural clot (Kenshi: config×0.1)
static constexpr float BLOOD_RECOVERY_RATE     = 0.02f;   // HP/s with active treatment
static constexpr float RESTING_HEAL_FLOOR      = 0.5f;    // HP/s on floor
static constexpr float RESTING_HEAL_BED        = 2.0f;    // HP/s in bed
static constexpr float RESTING_HEAL_DOCTOR_BED = 5.0f;    // HP/s in bed + doctor
static constexpr float DOCTOR_HEAL_MULT_PER_LV = 0.02f;  // +2% per field_medic level
static constexpr float KO_RECOVERY_S           = 60.f;    // seconds until KO ends unaided
static constexpr float KO_RECOVERY_DOCTOR_S    = 20.f;    // seconds with a doctor
// Kenshi RE: bleed_rate and bleeding_clot_rate are stored ×10 in config (×0.1 applied at load).
static constexpr float BLEED_CONFIG_SCALE      = 0.1f;    // multiply config value by this

// alignas(16) REMOVED (2026-09-08): was intended for bleed_rate[]'s
// _mm_load_ps(bleed_rate), but no such SIMD access exists anywhere in the
// codebase today (grepped engine/ + game/ for _mm_load_ps/_mm_store_ps --
// only particle_soa.cpp/transform_soa.cpp use them, both on separate SoA
// arrays, not this struct) -- the alignment was aspirational and unused.
// Confirmed via a minimal standalone gaia-ecs repro (see
// docs/GAIA_ALIGNAS_BUG.md) that gaia::ecs::World::add<T>(entity, value)
// segfaults (null m_pChunk deref inside ComponentSetter::sset,
// gaia.h:46061) for any alignas(16)+ component type added as a NON-FIRST
// component on an entity -- every real NPC's InjuryState is always added
// after other components, so this was a hard blocker under MD_ECS_GAIA.
// If _mm_load_ps(bleed_rate) is ever actually implemented, use
// _mm_loadu_ps (unaligned, same result, no alignment requirement) instead
// of re-adding alignas here.
struct InjuryState {
    float   bleed_rate[LIMB_COUNT];   // HP/s per limb (0=no bleed)   24B
    float   resting_heal_rate;        // HP/s current heal rate         4B
    float   ko_recovery_timer;        // seconds until KO ends; ≤0=ok   4B
    uint8_t splinted[LIMB_COUNT];     // 1 = splint applied to limb     6B
    uint8_t has_doctor;               // 1 = doctor NPC is adjacent      1B
    uint8_t doctor_skill;             // field_medic skill 0..99         1B
    uint8_t _pad[2];                  //                                 2B
};                                    //                         total  42B → padded to 44B
static_assert(sizeof(InjuryState) == 44, "InjuryState size (no longer alignas(16)-padded)");

// B-5: DeathShutdownSpeed — distance-based death animation budget (Kenshi RE).
// Graceful: full ragdoll + sound (<20m from player).
// Expedient: reduced ragdoll, no sound (20-50m).
// Critical: instant removal, no animation (>50m).
enum class DeathShutdownSpeed : uint8_t { Graceful = 0, Expedient = 1, Critical = 2 };

inline DeathShutdownSpeed GetDeathShutdownSpeed(float dist_sq) noexcept {
    if (dist_sq < 20.f * 20.f) return DeathShutdownSpeed::Graceful;
    if (dist_sq < 50.f * 50.f) return DeathShutdownSpeed::Expedient;
    return DeathShutdownSpeed::Critical;
}

// DeathState — tracks dying → dead → respawn cycle.
// Attach when InjuryState blood fraction drops below SurvivalConfig::death_threshold_frac.
// Kenshi RE: death_time@+0x544, death_threshold@+0x54c, min/max_respawn_time@+0x164/+0x2d.
struct DeathState {
    float               death_countdown_s   = 300.f;  // seconds remaining before permanent death
    float               respawn_countdown_s = 0.f;    // if >0: faction respawn pending
    DeathShutdownSpeed  death_speed         = DeathShutdownSpeed::Graceful;
    uint8_t             is_dying            = 0;      // 1 = in dying state (can be revived)
    uint8_t             is_dead             = 0;      // 1 = permanently dead
    uint8_t             _pad                = 0;
};
static_assert(sizeof(DeathState) == 12, "DeathState layout");

// Effective heal multiplier from doctor presence + skill.
inline float InjuryDoctorMult(const InjuryState& inj) noexcept {
    if (!inj.has_doctor) return 1.f;
    float m = 1.f + inj.doctor_skill * DOCTOR_HEAL_MULT_PER_LV;
    return (m > 3.f) ? 3.f : m;
}

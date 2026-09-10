#pragma once
#include <stdint.h>
#include <gaia.h>

// CharBodyState — per-entity body development state.
//
// Stores per-bone scale multipliers ({sx, sy, sz}) that drive body shape:
//   - bone_scales[b][3]: vertex deformation scale (setBoneSize equivalent)
//   - pos_scales[b][3]:  positional scale (setBonePositionalSize equivalent)
//
// Updated by BodyDevelopSystem (10 TPS) based on:
//   - CombatComponent: muscle development from fighting
//   - NeedsComponent:  fat from overeating, wasting from starvation
//   - StatSheet skills: strength, athletics → muscle groups
//
// Read by NpcRender → passed to OzzAnimator::Sample(... bone_scales)
// so in-game appearance matches character editor preview exactly.
//
// Initialized to identity ({1,1,1}) so default NPCs look unchanged.

static constexpr int BODY_MAX_BONES = 64;  // must equal OZZ_ANIM_MAX_BONES

// GAIA_STORAGE(Sparse) -- Phase 2 (prompt_/PROMPT_GAIA_MIGRATION.md §0/§4):
// one of the 5 components with a proven B3.4 risk. See world_transform.h's
// doc comment for the shared rationale.
struct CharBodyState {
    GAIA_STORAGE(Sparse);
    float bone_scales[BODY_MAX_BONES][3];  // vertex scale per bone, default {1,1,1}
    float pos_scales [BODY_MAX_BONES][3];  // positional scale, default {1,1,1}
    // Raw chardef body[4] Posture slider value (Kenshi range 0-70, neutral=35)
    // — NOT a scale, kept separately for OzzAnimator's posture pose-delta
    // (ApplyPostureDelta): NpcRender maps this to a time position on the
    // "postures" clip (t = clip_duration * posture * 0.01, matching Kenshi's
    // own setTimePosition(length*slider*0.01) — see char_customization.cpp).
    float posture = 35.f;

    void Reset() {
        for (int b = 0; b < BODY_MAX_BONES; ++b) {
            bone_scales[b][0] = bone_scales[b][1] = bone_scales[b][2] = 1.f;
            pos_scales [b][0] = pos_scales [b][1] = pos_scales [b][2] = 1.f;
        }
        posture = 35.f;
    }
};

// Immutable snapshot of CharBodyState::bone_scales as set by CharBodyState_InitFromDef.
// Used by BodyDevelopSystem: reset CharBodyState to baseline each tick, then
// multiply skill/hunger modifiers on top — avoids drift from accumulated deltas.
//
// GAIA_STORAGE(Sparse) -- Phase 2 (prompt_/PROMPT_GAIA_MIGRATION.md §0/§4):
// one of the 5 components with a proven B3.4 risk. See world_transform.h's
// doc comment for the shared rationale.
struct BodyBaseline {
    GAIA_STORAGE(Sparse);
    float bone_scales[BODY_MAX_BONES][3];
    float posture = 35.f;
};

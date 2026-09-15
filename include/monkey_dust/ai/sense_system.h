#pragma once
// M55 perception tick — moved to engine/ (all deps are engine types).
// PERF-14: md_rsqrtf/md_dist2d used instead of sqrtf for sense distance queries.
// SenseSystemUpdate(now_ms): call once per logic tick, after frame_flags dispatch.
// Visual cone: max contribution from ViewConeSet; Audio: linear falloff 15m.
// Rising edge on threshold_hi → writes last_activated_ms + last_known_x/z (Visual).
// VBfA-AI2: AwarenessLimits caps applied — prevents O(n²) reaction cascades.
// CATHODE RE §7: sense_cooldown_frames — per-NPC throttle when global budget exceeded.
#include <monkey_dust/platform/md_hints.h>
#include <monkey_dust/components/sense_component.h>
#include <monkey_dust/components/agent_state.h>
#include <monkey_dust/components/stealth_component.h>
#include <monkey_dust/components/noise_emitter.h>
#include <monkey_dust/world/world_transform.h>
#include <monkey_dust/ai/sense_registry.h>
#include <monkey_dust/ai/awareness_limits.h>
#include <monkey_dust/ecs/md_registry.h>
#include <monkey_dust/ai/ai_queries.h>
#include <monkey_dust/platform/job_system.h>
#include <cmath>
#ifdef __AVX2__
#  include <immintrin.h>
#elif defined(__SSE__)
#  include <xmmintrin.h>
#endif

static constexpr float SENSE_AUDIO_RADIUS_M = 15.0f;
static constexpr float SENSE_PI             = 3.14159265f;
static constexpr float SENSE_RAD2DEG        = 180.0f / SENSE_PI;

// Inverse 4th-power distance falloff.
// (range/dist)^4 — double distance → 1/16th activation.
// Used for audio/smell/vibration where ultra-local effect is desired.
// Linear (old): 1 - dist/range   → gentle slope
// r⁴   (VBfA): (range/dist)^4   → steep cliff, very local
static inline float sense_falloff_r4(float dist, float range) {
    if (dist <= 0.f)    return 1.f;
    if (dist >= range)  return 0.f;
    float ratio = range / dist;
    float r2    = ratio * ratio;
    float r4    = r2 * r2;
    return r4 > 1.f ? 1.f : r4;  // clamp: at dist≈0 r4→∞, cap at 1.0
}

static inline float sense_wrap_angle(float a) {
    while (a >  SENSE_PI) a -= 2.0f * SENSE_PI;
    while (a < -SENSE_PI) a += 2.0f * SENSE_PI;
    return a;
}

static inline float sense_cone_activation(const ViewCone& cone,
                                          float dist, float angle_abs_deg) {
    if (dist > cone.length_m)                    return 0.f;
    if (angle_abs_deg > cone.h_angle_deg * 0.5f) return 0.f;
    float t = 1.f - dist / cone.length_m;
    return cone.dist_lo + t * (cone.dist_hi - cone.dist_lo);
}

// ARCHITECTURE_IDEAS.md #6 — visual/audio activation math parallelized across
// JobSystem workers. Per JobSystem's own documented contract (job_system.h:
// "jobs must not touch EnTT registry... Safe for: ... sense distance
// queries"), each job operates ONLY on a flat POD SenseJobInput — no entt
// access, no shared mutable state (unlike the naive "just parallelize
// bt_view.each()" approach rejected earlier: AIBudget::TryConsume and
// AwarenessLimits::g_frame's reaction caps are non-atomic check-then-add
// counters that would race under concurrent writers — see ARCHITECTURE_IDEAS.md
// #6 status). The reaction-cap logic (order-sensitive: "first N to activate
// reserve the reaction") stays serial in the scatter phase below, in the
// same entity-iteration order as before, so its semantics are unchanged —
// only the per-entity floating-point activation math (cone/r⁴ falloff) runs
// on worker threads.
static constexpr int MAX_SENSE_JOBS = 512;

struct SenseJobInput {
    MdEntity e = MdEntity::Null();
    float px = 0.f, pz = 0.f, rot_y = 0.f;
    float player_x = 0.f, player_z = 0.f, player_stealth = 1.f;
    uint8_t cone_set_idx = 0;
    float vis_range_mult = 1.f, audio_range_mult = 1.f, fill_mult = 1.f;
    float darkness_mult = 1.f, crouch_mult = 1.f, noise_mult = 1.f;
    bool  is_crouching = false;
    // outputs, written by eval_sense_job on a worker thread
    float visual_act = 0.f;
    float audio_act  = 0.f;
};

static SenseJobInput s_sense_jobs[MAX_SENSE_JOBS];
static int           s_sense_count = 0;

// Runs on a JobSystem worker thread. Pure math over j's POD fields —
// no entt::registry access, no shared mutable state. See sense_wrap_angle/
// sense_cone_activation/sense_falloff_r4 above (already free functions).
inline void eval_sense_job(void* p) {
    auto* j = static_cast<SenseJobInput*>(p);

    float dx   = j->player_x - j->px, dz = j->player_z - j->pz;
    float dist = md_dist2d(dx, dz);
    float angle_to   = atan2f(dx, dz);
    float angle_diff = fabsf(sense_wrap_angle(angle_to - j->rot_y)) * SENSE_RAD2DEG;

    float visual_act = 0.f;
    float eff_vis_range = AwarenessLimits::MAX_VISION_RANGE * j->vis_range_mult;
    if (dist <= eff_vis_range) {
        const ViewConeSet* vcs = SenseRegistry::Get().At(j->cone_set_idx);
        if (vcs) {
            float stance_factor = j->is_crouching ? j->crouch_mult : 1.f;
            for (int c = 0; c < vcs->cone_count; ++c) {
                ViewCone scaled = vcs->cones[c];
                scaled.length_m *= j->vis_range_mult;
                float contrib = sense_cone_activation(scaled, dist, angle_diff);
                contrib *= j->fill_mult * j->darkness_mult * stance_factor;
                if (contrib > visual_act) visual_act = contrib;
            }
        }
    }
    visual_act *= j->player_stealth;

    float eff_audio_range = SENSE_AUDIO_RADIUS_M * j->audio_range_mult;
    float audio_act = sense_falloff_r4(dist, eff_audio_range) * j->fill_mult * j->noise_mult
                    * j->player_stealth;

    j->visual_act = visual_act;
    j->audio_act  = audio_act;
}

inline void SenseSystemUpdate(float now_ms) {
    // VBfA-AI2: reset per-tick reaction caps (prevents O(n²) alert cascades)
    AwarenessLimits::g_frame.Reset();

    auto& reg = MdRegistry::Get();

    MdEntity player = MdEntity::Null();
    MdEach(ai_queries::AgentStateOnly(), [&](MdEntity e, const AgentState& as) {
        if (player == MdEntity::Null() && as.lcflags.test(lcf::IS_PLAYER))
            player = e;
    });
    if (player == MdEntity::Null()) return;

    const WorldTransform* pwt = reg.Handle(player).try_get_mut<WorldTransform>();
    if (!pwt) return;

    // B-1: StealthComponent on player reduces all observer activation fills.
    const StealthComponent* psc = reg.Handle(player).try_get_mut<StealthComponent>();
    float player_stealth = psc ? psc->stealth_factor : 1.f;

    auto uint32_now = static_cast<uint32_t>(now_ms);

    // ── Gather (serial, main thread): flatten per-entity inputs; cooldown
    // throttle stays here since it mutates sc.sense_cooldown_frames per-entity
    // (cheap, and keeps the "who gets a job this tick" decision in one place).
    s_sense_count = 0;
    MdEach(ai_queries::SenseWorldTransformAgentState(), [&](
        MdEntity e, SenseComponent& sc,
        const WorldTransform& wt, AgentState& as)
    {
        if (as.lcflags.test(lcf::IS_PLAYER)) return;

        // CATHODE RE §7: NPC_SenseLimiter — per-NPC budget throttle.
        // If cooldown > 0, skip sense queries and decrement. Yields ~15% CPU at 300 NPCs.
        if (sc.sense_cooldown_frames > 0) { --sc.sense_cooldown_frames; return; }
        if (s_sense_count >= MAX_SENSE_JOBS) return;  // budget hit — remaining NPCs skip this tick

        // AI-4: read optional SenseModifiers (CATHODE RE §6.4 config-driven ranges).
        // If no SenseModifiers component, defaults (1.0) apply — same as before.
        const SenseModifiers* sm = reg.Handle(e).try_get_mut<SenseModifiers>();

        SenseJobInput& j = s_sense_jobs[s_sense_count++];
        j.e              = e;
        j.px             = wt.x;
        j.pz             = wt.z;
        j.rot_y          = wt.rot_y;
        j.player_x       = pwt->x;
        j.player_z       = pwt->z;
        j.player_stealth = player_stealth;
        j.cone_set_idx   = sc.cone_set_idx;
        j.vis_range_mult    = sm ? sm->visual_range_mult    : 1.f;
        j.audio_range_mult  = sm ? sm->audio_range_mult     : 1.f;
        j.fill_mult         = sm ? sm->activation_fill_mult : 1.f;
        j.darkness_mult     = sm ? sm->darkness_mult        : 1.f;
        j.crouch_mult       = sm ? sm->crouch_mult          : 1.f;
        j.noise_mult        = sm ? sm->noise_mult           : 1.f;
        j.is_crouching      = (as.locomotion_state == LocomotionState::Crouching);
        j.visual_act = 0.f;
        j.audio_act  = 0.f;
    });

    // ── Parallel (JobSystem workers): pure math over POD SenseJobInput —
    // no registry access, matches job_system.h's documented safe use case.
    // NumWorkers()==0 means JobSystem::Init() was never called (e.g. test
    // binaries with no game bootstrap) -- Submit()/Flush() on an
    // uninitialized JobSystem (null mutex/condvars) hangs forever, since
    // inflight_ is incremented but no worker thread exists to decrement it.
    // Fall back to synchronous evaluation on the calling thread instead of
    // hard-requiring every consumer to call JobSystem::Init() first.
    if (JobSystem::Get().NumWorkers() > 0) {
        for (int i = 0; i < s_sense_count; ++i)
            JobSystem::Get().Submit(eval_sense_job, &s_sense_jobs[i]);
        JobSystem::Get().Flush();
    } else {
        for (int i = 0; i < s_sense_count; ++i)
            eval_sense_job(&s_sense_jobs[i]);
    }

    // ── Scatter (serial, main thread): write results back to SenseComponent,
    // apply the order-sensitive reaction-cap logic in the SAME iteration
    // order as the gather pass — semantics identical to the pre-parallel
    // version, just split into gather/compute/scatter phases.
    for (int i = 0; i < s_sense_count; ++i) {
        const SenseJobInput& j = s_sense_jobs[i];
        SenseComponent& sc = reg.Handle(j.e).get_mut<SenseComponent>();

        bool vis_was_hi = sc.activation[0] >= sc.threshold_hi;
        sc.activation[0] = j.visual_act;
        if (!vis_was_hi && j.visual_act >= sc.threshold_hi) {
            // VBfA-AI2: cap at MAX_REACT_TO_PLAYER per tick.
            if (AwarenessLimits::g_frame.reacted_to_player
                    < AwarenessLimits::MAX_REACT_TO_PLAYER) {
                sc.last_activated_ms[0] = uint32_now;
                sc.last_known_x = pwt->x;
                sc.last_known_z = pwt->z;
                ++AwarenessLimits::g_frame.reacted_to_player;

                // CATHODE RE §7.8: raise awareness watermark when NPC fully detects player.
                AgentBlackboard* bb = reg.Handle(j.e).try_get_mut<AgentBlackboard>();
                if (bb && bb->awareness_watermark < AwarenessState::Aware) {
                    bb->awareness_watermark = AwarenessState::Aware;
                    bb->watermark_ms = uint32_now;
                }
            }
        }

        bool aud_was_hi = sc.activation[1] >= sc.threshold_hi;
        sc.activation[1] = j.audio_act;
        if (!aud_was_hi && j.audio_act >= sc.threshold_hi) {
            if (AwarenessLimits::g_frame.reacted_to_combat
                    < AwarenessLimits::MAX_REACT_TO_COMBAT) {
                sc.last_activated_ms[1] = uint32_now;
                ++AwarenessLimits::g_frame.reacted_to_combat;
            }
        }

        // Clamp all MAX_SENSES activations to [0, 1].
        // B-4: MAX_SENSES=10; AVX2 handles 8+2, SSE handles 4+4+2.
#ifdef __AVX2__
        {
            __m256 zero8 = _mm256_setzero_ps();
            __m256 one8  = _mm256_set1_ps(1.f);
            float* a = sc.activation;
            _mm256_storeu_ps(a, _mm256_min_ps(_mm256_max_ps(_mm256_loadu_ps(a), zero8), one8));
            for (int k = 8; k < MAX_SENSES; ++k) {
                if (a[k] < 0.f) a[k] = 0.f;
                if (a[k] > 1.f) a[k] = 1.f;
            }
        }
#elif defined(__SSE__)
        {
            __m128 zero4 = _mm_setzero_ps();
            __m128 one4  = _mm_set1_ps(1.f);
            float* a = sc.activation;
            _mm_storeu_ps(a + 0, _mm_min_ps(_mm_max_ps(_mm_loadu_ps(a + 0), zero4), one4));
            _mm_storeu_ps(a + 4, _mm_min_ps(_mm_max_ps(_mm_loadu_ps(a + 4), zero4), one4));
            for (int k = 8; k < MAX_SENSES; ++k) {
                if (a[k] < 0.f) a[k] = 0.f;
                if (a[k] > 1.f) a[k] = 1.f;
            }
        }
#else
        for (int k = 0; k < MAX_SENSES; ++k) {
            if (sc.activation[k] < 0.f) sc.activation[k] = 0.f;
            if (sc.activation[k] > 1.f) sc.activation[k] = 1.f;
        }
#endif
    }

    // B-3: NoiseEmitter pass — fill activation[AudioCombat=1] and activation[AudioMovement=2]
    // from nearby noise sources. Separate from the player detection path above.
    MdEach(ai_queries::NoiseWorldTransform(), [&](
        const NoiseEmitter& ne, const WorldTransform& nwt)
    {
        if (ne.noise_radius_m <= 0.f) return;
        float r2 = ne.noise_radius_m * ne.noise_radius_m;
        MdEach(ai_queries::SenseWorldTransform(), [&](
            SenseComponent& sc, const WorldTransform& owt)
        {
            float dx = nwt.x - owt.x, dz = nwt.z - owt.z;
            float d2 = dx*dx + dz*dz;
            if (d2 >= r2) return;
            float contrib = 1.f - d2 / r2;  // linear falloff
            auto nt = static_cast<NoiseType>(ne.noise_type);
            if (nt == NoiseType::Weapon || nt == NoiseType::Explosion)
                sc.activation[(int)SenseType::AudioCombat]   += contrib;
            if (nt == NoiseType::Footstep || nt == NoiseType::Vent)
                sc.activation[(int)SenseType::AudioMovement] += contrib;
            if (nt == NoiseType::Voice) {  // voice activates both at half strength
                sc.activation[(int)SenseType::AudioCombat]   += contrib * 0.5f;
                sc.activation[(int)SenseType::AudioMovement] += contrib * 0.5f;
            }
        });
    });

    // B-3: SmellEmitter pass — fill activation[Smell=3] from nearby smell sources.
    MdEach(ai_queries::SmellWorldTransform(), [&](
        const SmellEmitter& se, const WorldTransform& swt)
    {
        if (se.smell_radius_m <= 0.f) return;
        float r2 = se.smell_radius_m * se.smell_radius_m;
        float intensity = (float)se.intensity / 255.f;
        MdEach(ai_queries::SenseWorldTransform(), [&](
            SenseComponent& sc, const WorldTransform& owt)
        {
            float dx = swt.x - owt.x, dz = swt.z - owt.z;
            float d2 = dx*dx + dz*dz;
            if (d2 >= r2) return;
            sc.activation[(int)SenseType::Smell] += (1.f - d2 / r2) * intensity;
        });
    });
}

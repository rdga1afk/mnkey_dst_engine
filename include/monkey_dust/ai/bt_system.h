#pragma once
#include <monkey_dust/ecs/engine_context.h>
#include <monkey_dust/ecs/md_registry.h>
#include <monkey_dust/components/agent_state.h>
#include <monkey_dust/components/bt_components.h>
#include <cstdint>

// ── BTSystem ──────────────────────────────────────────────────────────────────
// Main BT execution loop.
// Call Tick() once per logic tick (10 TPS).
//
// Per-entity sequence:
//   1. Clear AgentState::frame_flags (C13 invariant — single-tick signals reset)
//   2. Tick DirectorHintComponent: expire stale hints (>MAX_PENDING_TICKS)
//   3. Tiered LOD skip (VBfA/CATHODE RE): entities with motivation==Dormant or
//      distance > TIER_FAR_M are skipped every N frames (every 5/10/20 logic ticks).
//   4. If BehaviorTreeComponent::enabled && tree valid → BehaviorTree::tick()
//
// Tiered tick rates (VBfA RE: 5/10/20 frame modulo pattern):
//   TIER_NEAR  (< TIER_MED_M)  : every tick (full update)
//   TIER_MED   (< TIER_FAR_M)  : every 10 ticks (~1 s)
//   TIER_FAR   (>= TIER_FAR_M) : every 20 ticks (~2 s)
//   DORMANT (motivation==Dormant): every 20 ticks, minimal update only
//
// Thread safety: single-threaded. All entities ticked sequentially on the
// main logic thread. Do NOT call from render thread.
//
// Integration:
//   BTSystem bt_sys;
//   // In logic tick:
//   bt_sys.Tick(ctx, registry, nowMs);
//
// NOT on the live game's critical path (B3.4 note): AISystem::Update
// (game/src/ai/ai_system.h) ticks BTs directly via MdRegistry::View<>(),
// bypassing BTSystem entirely. BTSystem is exercised by tools/flare_demo
// and its own unit test suite only.

static constexpr float BT_TIER_NEAR_M = 30.f;   // full tick within 30 m
static constexpr float BT_TIER_MED_M  = 80.f;   // reduced tick up to 80 m
// beyond 80 m → every 20 ticks; Dormant → every 20 ticks regardless

class BTSystem {
public:
    // nowMs: current game time in milliseconds (for TimerStart/TimerCheck nodes).
    // ctx:   engine context (frame_index for WeightedSelector RNG, delta_time, etc.)
    // reg:   world — queries (AgentState + BehaviorTreeComponent).
    void Tick(md::EngineContext& ctx, MdWorldRef& reg, uint32_t nowMs);

    uint32_t frame_idx() const noexcept { return frame_idx_; }

    // gaia's observer callback shape is Iter&-based (no direct
    // (entity, T&) form the way flecs's .each() supports) -- see
    // hierarchy_utils.cpp's port for the same pattern applied first.
    static void OnComponentDestroy(gaia::ecs::Iter& it);

    static void ConnectRegistry(MdWorldRef& reg) {
        reg.observer().event(gaia::ecs::ObserverEvent::OnDel)
            .all<BehaviorTreeComponent&>()
            .on_each(OnComponentDestroy);
    }

private:
    uint32_t frame_idx_ = 0;  // incremented each Tick(); used for tiered modulo skip
};

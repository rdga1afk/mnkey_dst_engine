#include <monkey_dust/ai/bt_system.h>
#include <monkey_dust/ai/ai_budget.h>
#include <monkey_dust/components/ai_agent.h>
#include <monkey_dust/world/world_transform.h>
#include <monkey_dust/world/transform_soa.h>

//
// VBfA ran at 60 fps; monkey_dust logic at 10 TPS → scale modulos by /6.
// Result: update intervals grow logarithmically with distance (not linearly).
//
// Tier │ dist (m) │ modulo │ update interval │ VBfA orig modulo
// ─────┼──────────┼────────┼─────────────────┼─────────────────
//   0  │    < 15  │    1   │  100ms (full)   │    1
//   1  │  15–30   │    5   │  500ms           │   ~30
//   2  │  30–50   │   10   │    1 s           │   63
//   3  │  50–80   │   20   │    2 s           │  106
//   4  │  80–120  │   42   │  ~4.2 s          │  255 (VBfA tier 1)
//   5  │ 120–160  │   63   │  ~6.3 s          │  382
//   6  │ 160–220  │  106   │  ~10.6 s         │  636
//   7  │ 220–300  │  127   │  ~12.7 s         │  763
//   8  │   > 300  │  190   │  ~19 s           │ 1144 (VBfA tier 6, "nearly dormant")
//
// Dormant motivation → forced tier 8 regardless of distance.
// Entity-id spread: (fi + entity_id) % modulo == 0  → evenly distributes load.

struct BtLodTier { float dist_sq; uint32_t modulo; };
static constexpr BtLodTier BT_LOD_TIERS[] = {
    {  15.f *  15.f,   1u },   // tier 0 — full rate
    {  30.f *  30.f,   5u },   // tier 1
    {  50.f *  50.f,  10u },
    {  80.f *  80.f,  15u },
    { 120.f * 120.f,  20u },
    { 160.f * 160.f,  42u },   // tier 5
    { 220.f * 220.f,  63u },   // tier 6
    { 300.f * 300.f, 106u },   // tier 7
    { 400.f * 400.f, 127u },   // tier 8
};
static constexpr uint32_t BT_LOD_FAR_MODULO   = 190u;   // tier 9: > 400m (~19s at 10TPS)
static constexpr uint32_t BT_LOD_ULTRA_MODULO = 960u;

// Legacy constants kept for other systems that reference them.
static constexpr float BT_LOD_TIER1_SQ = 30.0f * 30.0f;
static constexpr float BT_LOD_TIER2_SQ = 80.0f * 80.0f;

void BTSystem::Tick(md::EngineContext& ctx, flecs::world& reg, uint32_t nowMs) {
    ++frame_idx_;

    // B3.4: the periodic near-first sort (every 190 frames, via
    // entt::registry::sort<BehaviorTreeComponent>()) is dropped under
    // flecs. entt::sort<T>() physically reordered a component pool once,
    // benefiting every subsequent view until the next sort call — flecs's
    // equivalent (order_by) is attached to a SPECIFIC query and reapplied
    // lazily each time THAT query iterates, which doesn't fit a query
    // that's rebuilt fresh every Tick() call the way bt_view below is.
    // Not on the live game's critical path (see bt_system.h) — the loop
    // stays functionally correct, just without the cache-locality
    // micro-optimization.

    // Phase 1: clear frame_flags + expire stale DirectorHints
    //
    // Task #7/#8 concurrency-audit follow-up: these 3 queries used to be
    // rebuilt (reg.query<...>()) on EVERY Tick() call — an on-the-fly
    // query construction each time, the same hazardous pattern MdView
    // avoids via a function-local static. Made static here too: every
    // call site across the whole codebase (tests + tools/flare_demo)
    // passes MdRegistry::Get().Raw() as `reg`, so it's provably always
    // the same flecs::world singleton — caching against `reg` once is
    // safe. BTSystem::Tick is not on the live game's critical path today
    // (see bt_system.h — AISystem::Update bypasses it entirely) so this
    // wasn't reachable from the concurrent JobGraph wave, but closing it
    // now removes the landmine for whenever it IS wired in.
    static auto hint_view = reg.query<AgentState, DirectorHintComponent>();
    hint_view.each([&](flecs::entity, AgentState& as, DirectorHintComponent& hint) {
        // C13: clear per-frame signals before BT tick
        as.frame_flags   = 0;
        as.npc_tick_cost = 0;  // VBfA: reset per-NPC work budget each tick
        // Stale hint expiry — prevents Director hints from blocking BT indefinitely
        if (hint.role_pending) {
            if (++hint.pending_ticks > DirectorHintComponent::MAX_PENDING_TICKS) {
                hint.role_pending  = false;
                hint.pending_ticks = 0;
            }
        }
    });

    // Phase 2: clear frame_flags for entities with no DirectorHintComponent
    static auto bare_view = reg.query_builder<AgentState>().without<DirectorHintComponent>().build();
    bare_view.each([](flecs::entity, AgentState& as) {
        as.frame_flags   = 0;
        as.npc_tick_cost = 0;
    });

    // Phase 3: tick active behavior trees with distance-based LOD.
    // Frame_flags already cleared in phases 1+2 for ALL entities regardless of LOD.
    const auto& tsoa = TransformSoA::Get();
    const uint32_t fi = frame_idx_;
    static auto bt_view = reg.query<AgentState, BehaviorTreeComponent>();
    bt_view.each([&](flecs::entity e, AgentState& as, BehaviorTreeComponent& btc) {
        if (!btc.enabled || !btc.tree || !btc.tree->isValid()) return;
        if (as.lcflags.test(lcf::IS_SUSPENDED)) return;  // Batch 11 P8: suspension gate
        if (as.npc_dormant) return;                       // VBfA: external area-manager sleep
        if (as.npc_tick_cost >= NPC_TICK_COST_MAX) return; // VBfA: per-NPC over-work guard

        const uint32_t eid = (uint32_t)e.id();

        if (as.motivation == MotivationType::Dormant) {
            // Ultra-rare tier: OffscreenNpcDatabase handles 0.1Hz; BT near-dormant @ 19s.
            if ((fi + eid) % BT_LOD_FAR_MODULO != 0u) return;
        } else {
            const auto* wt = e.try_get<WorldTransform>();
            if (wt && wt->slot < (uint32_t)tsoa.active_count) {
                float dsq = tsoa.dist_sq[wt->slot];
                uint32_t modulo = BT_LOD_FAR_MODULO;  // default: far tier
                for (const auto& t : BT_LOD_TIERS) {
                    if (dsq <= t.dist_sq) { modulo = t.modulo; break; }
                }

                // Applied on top of distance modulo for better load spreading.
                if (modulo > 1u) {
                    if ((fi + eid) % modulo != 0u) return;
                } else {
                    // Full-rate (tier 0, < 15m): still stagger by %7 to avoid frame spike.
                    if (eid % 7u != fi % 7u) return;
                }
            }
        }


        // Player is exempt; schedule NPCs (bt_template_id==255) cost 0.2;
        // all other NPCs cost 1.0 (default combat unit).
        float cost = BtBudgetCost::kGuard;
        if (as.lcflags.test(lcf::IS_PLAYER)) {
            cost = BtBudgetCost::kPlayer;
        } else {
            const AIAgent* ag = e.try_get<AIAgent>();
            if (ag && ag->bt_template_id == 255)
                cost = BtBudgetCost::kScheduleNpc;
        }
        if (!AIBudget::Get().TryConsume(cost)) return;  // budget exhausted: skip BT
        as.npc_tick_cost += static_cast<uint16_t>(cost * 100.f);  // VBfA: accumulate work

        btc.tree->tick(ctx, MdEntity(e.id()), &as, nowMs);
    });
}

void BTSystem::OnComponentDestroy(flecs::entity e, BehaviorTreeComponent& btc) {
    if (btc.owning && btc.tree) {
        delete btc.tree;
        btc.tree = nullptr;
    }
}

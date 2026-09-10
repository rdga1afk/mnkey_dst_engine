#include <monkey_dust/ai/ai_queries.h>
#include <monkey_dust/ai/squad_controller.h>

namespace ai_queries {

// Mutability audited from every real MdEach consumer across the codebase
// (docs/GAIA_SEAM_AUDIT.md follow-up, not guessed): consumer list per
// function is in the git history / grep `ai_queries::<Name>(` -- summarized
// per function below.

// Consumers: logic_tick_needs_ai_nav.cpp (NpcNeeds&, mutates hunger/
// fatigue/morale), needs_ai_snapshot.h (const NpcNeeds&) -- union: mutable.
#if defined(MD_ECS_GAIA)
gaia::ecs::Query& NpcNeedsOnly() {
    static auto q = MdRegistry::Get().Raw().query().all<NpcNeeds&>();
    return q;
}
#else
flecs::query<NpcNeeds>& NpcNeedsOnly() {
    static auto q = MdRegistry::Get().Raw().query<NpcNeeds>();
    return q;
}
#endif

// Consumer: logic_tick_needs_ai_nav.cpp -- BleedComponent& bc, LimbHealth& lh.
#if defined(MD_ECS_GAIA)
gaia::ecs::Query& BleedLimbHealth() {
    static auto q = MdRegistry::Get().Raw().query().all<BleedComponent&>().all<LimbHealth&>();
    return q;
}
#else
flecs::query<BleedComponent, LimbHealth>& BleedLimbHealth() {
    static auto q = MdRegistry::Get().Raw().query<BleedComponent, LimbHealth>();
    return q;
}
#endif

// Consumer: logic_tick_needs_ai_nav.cpp -- InjuryState& inj, LimbHealth& lh.
#if defined(MD_ECS_GAIA)
gaia::ecs::Query& InjuryLimbHealth() {
    static auto q = MdRegistry::Get().Raw().query().all<InjuryState&>().all<LimbHealth&>();
    return q;
}
#else
flecs::query<InjuryState, LimbHealth>& InjuryLimbHealth() {
    static auto q = MdRegistry::Get().Raw().query<InjuryState, LimbHealth>();
    return q;
}
#endif

// Consumer: schedule_system.h -- AgentState& as, const NpcSchedule& sched.
#if defined(MD_ECS_GAIA)
gaia::ecs::Query& AgentStateSchedule() {
    static auto q = MdRegistry::Get().Raw().query().all<AgentState&>().all<NpcSchedule>();
    return q;
}
#else
flecs::query<AgentState, NpcSchedule>& AgentStateSchedule() {
    static auto q = MdRegistry::Get().Raw().query<AgentState, NpcSchedule>();
    return q;
}
#endif

// Consumer: schedule_system.h -- AIAgent& ai, BTComponent& btc.
#if defined(MD_ECS_GAIA)
gaia::ecs::Query& AIAgentBT() {
    static auto q = MdRegistry::Get().Raw().query().all<AIAgent&>().all<BTComponent&>();
    return q;
}
#else
flecs::query<AIAgent, BTComponent>& AIAgentBT() {
    static auto q = MdRegistry::Get().Raw().query<AIAgent, BTComponent>();
    return q;
}
#endif

// Consumer: squad_controller.h -- SquadController& sc.
#if defined(MD_ECS_GAIA)
gaia::ecs::Query& SquadControllers() {
    static auto q = MdRegistry::Get().Raw().query().all<SquadController&>();
    return q;
}
#else
flecs::query<SquadController>& SquadControllers() {
    static auto q = MdRegistry::Get().Raw().query<SquadController>();
    return q;
}
#endif

// task #8 Phase 4: order_by<AIAgent> replaces the old EnTT-era
// MdRegistry::Sort<AIAgent>() (a periodic, timer-gated pool-wide physical
// reorder — a no-op ever since the B3.4 flecs swap, since flecs has no
// pool to physically reorder the way entt did). flecs's per-query
// order_by does the equivalent job properly instead: entities of the same
// faction_id cluster together in this query's iteration order (the actual
// benefit logic_tick.cpp's old comment described — faction cache-locality
// for BT/combat evaluation), and per flecs.h's own doc comment on
// order_by, re-sorting only happens "if that component has changed, or
// when the entity order in the table has changed" — i.e. automatically
// exactly when needed (an AIAgent spawned/despawned/its faction_id
// written), not on a blind 190-tick timer that could leave a freshly
// spawned NPC unsorted for up to ~19s or re-sort for nothing when
// nothing changed.

// CompareAIAgentFactionGaia removed 2026-09-08 along with gaia's
// .sort_by<AIAgent>() below (GATE 4 root-cause fix) -- no longer called
// under MD_ECS_GAIA.
#if !defined(MD_ECS_GAIA)
static int CompareAIAgentFaction(flecs::entity_t, const AIAgent* a,
                                  flecs::entity_t, const AIAgent* b) {
    return (a->faction_id > b->faction_id) - (a->faction_id < b->faction_id);
}
#endif

// Consumer: ai_system.h -- AIAgent& ai, BTComponent& btc, WorldTransform& wt,
// AIAgentTickState& ts (all mutable). sort_by<T> ported as-is here (kept
// the 4-term split, NOT simplified) -- prompt_/PROMPT_GAIA_MIGRATION.md §7
// p.1 is where the 4th-term removal (confirmed unnecessary under gaia by
// probe P-G, docs/GAIA_MIGRATION_ANALYSIS.md UNKNOWN-2) belongs, not this
// mechanical query-syntax pass.
//
// .sort_by<AIAgent>() REMOVED under gaia (2026-09-08, GATE 4 root-cause):
// the flecs .order_by<AIAgent> this was ported "as-is" from is a pure
// cache-locality optimization (faction-grouped iteration for BT/combat
// evaluation, ai_system.h's only consumer -- no code depends on iteration
// ORDER for correctness, verified by grep across engine/+game/). Under
// gaia it cost 45.76% of ALL sampled CPU cycles at 512 NPCs
// (perf-diffed against flecs's 0.37% for the equivalent
// flecs_query_cache_sort_table_generic -- see CLAUDE_STATE.md's GATE 4
// entry for the full profile comparison): sort_entities_inter's
// "quicksort across chunks" resolves each element via
// get_flat_comp_ptr, an O(chunks-in-archetype) linear scan per access
// (gaia's own TODO above sort_entities() calls this "not optimal,
// makes sorting more expensive") -- O(n^2 log n / C) overall, not
// O(n log n). A cache-locality micro-optimization that costs
// quadratically more than it could ever save is not an optimization;
// dropped rather than reimplemented as a hand-rolled periodic sort.
#if defined(MD_ECS_GAIA)
gaia::ecs::Query& AIAgentBTWorldTransform() {
    static auto q = MdRegistry::Get().Raw()
        .query().all<AIAgent&>().all<BTComponent&>().all<WorldTransform&>().all<AIAgentTickState&>();
    return q;
}
#else
flecs::query<AIAgent, BTComponent, WorldTransform, AIAgentTickState>& AIAgentBTWorldTransform() {
    // AIAgentTickState is a 4th term alongside AIAgent, not folded into it
    // (see ai_agent.h's doc comment on the type) -- writing it every tick
    // must NOT dirty AIAgent's own change monitor, which order_by<AIAgent>
    // below depends on staying clean while JobGraph runs this batch in
    // multithreaded mode (audit S1-00, 2026-08-27).
    static auto q = MdRegistry::Get().Raw()
        .query_builder<AIAgent, BTComponent, WorldTransform, AIAgentTickState>()
        .order_by<AIAgent>(CompareAIAgentFaction)
        .build();
    return q;
}
#endif

// Consumer: frame_flag_dispatch.h -- AgentState& as, WorldTransform& wt.
#if defined(MD_ECS_GAIA)
gaia::ecs::Query& AgentStateWorldTransform() {
    static auto q = MdRegistry::Get().Raw().query().all<AgentState&>().all<WorldTransform&>();
    return q;
}
#else
flecs::query<AgentState, WorldTransform>& AgentStateWorldTransform() {
    static auto q = MdRegistry::Get().Raw().query<AgentState, WorldTransform>();
    return q;
}
#endif

// Consumer: sense_system.h -- const AgentState& as.
#if defined(MD_ECS_GAIA)
gaia::ecs::Query& AgentStateOnly() {
    static auto q = MdRegistry::Get().Raw().query().all<AgentState>();
    return q;
}
#else
flecs::query<AgentState>& AgentStateOnly() {
    static auto q = MdRegistry::Get().Raw().query<AgentState>();
    return q;
}
#endif

// Consumer: sense_system.h -- SenseComponent& sc, const WorldTransform& wt,
// AgentState& as.
#if defined(MD_ECS_GAIA)
gaia::ecs::Query& SenseWorldTransformAgentState() {
    static auto q = MdRegistry::Get().Raw().query().all<SenseComponent&>().all<WorldTransform>().all<AgentState&>();
    return q;
}
#else
flecs::query<SenseComponent, WorldTransform, AgentState>& SenseWorldTransformAgentState() {
    static auto q = MdRegistry::Get().Raw().query<SenseComponent, WorldTransform, AgentState>();
    return q;
}
#endif

// Consumer: sense_system.h -- const NoiseEmitter& ne, const WorldTransform& nwt.
#if defined(MD_ECS_GAIA)
gaia::ecs::Query& NoiseWorldTransform() {
    static auto q = MdRegistry::Get().Raw().query().all<NoiseEmitter>().all<WorldTransform>();
    return q;
}
#else
flecs::query<NoiseEmitter, WorldTransform>& NoiseWorldTransform() {
    static auto q = MdRegistry::Get().Raw().query<NoiseEmitter, WorldTransform>();
    return q;
}
#endif

// Consumer: sense_system.h (used twice) -- SenseComponent& sc, const WorldTransform& owt.
#if defined(MD_ECS_GAIA)
gaia::ecs::Query& SenseWorldTransform() {
    static auto q = MdRegistry::Get().Raw().query().all<SenseComponent&>().all<WorldTransform>();
    return q;
}
#else
flecs::query<SenseComponent, WorldTransform>& SenseWorldTransform() {
    static auto q = MdRegistry::Get().Raw().query<SenseComponent, WorldTransform>();
    return q;
}
#endif

// Consumer: sense_system.h -- const SmellEmitter& se, const WorldTransform& swt.
#if defined(MD_ECS_GAIA)
gaia::ecs::Query& SmellWorldTransform() {
    static auto q = MdRegistry::Get().Raw().query().all<SmellEmitter>().all<WorldTransform>();
    return q;
}
#else
flecs::query<SmellEmitter, WorldTransform>& SmellWorldTransform() {
    static auto q = MdRegistry::Get().Raw().query<SmellEmitter, WorldTransform>();
    return q;
}
#endif

// Consumer: combat_dispatch.h -- AgentState& as, AgentBlackboard& bb,
// Combat& cbt, const WorldTransform& wt.
#if defined(MD_ECS_GAIA)
gaia::ecs::Query& AgentFullCombat() {
    static auto q = MdRegistry::Get().Raw().query().all<AgentState&>().all<AgentBlackboard&>().all<Combat&>().all<WorldTransform>();
    return q;
}
#else
flecs::query<AgentState, AgentBlackboard, Combat, WorldTransform>& AgentFullCombat() {
    static auto q = MdRegistry::Get().Raw().query<AgentState, AgentBlackboard, Combat, WorldTransform>();
    return q;
}
#endif

// Consumer: ai_goal_needs.cpp -- const Inventory& ci, const WorldTransform& tr.
#if defined(MD_ECS_GAIA)
gaia::ecs::Query& InventoryWorldTransform() {
    static auto q = MdRegistry::Get().Raw().query().all<Inventory>().all<WorldTransform>();
    return q;
}
#else
flecs::query<Inventory, WorldTransform>& InventoryWorldTransform() {
    static auto q = MdRegistry::Get().Raw().query<Inventory, WorldTransform>();
    return q;
}
#endif

// Consumers: ai_goal_needs.cpp (x3), ai_goal_vendor.cpp (x1) -- all
// const Building&, const WorldTransform& (verified all 4 call sites).
#if defined(MD_ECS_GAIA)
gaia::ecs::Query& BuildingWorldTransform() {
    static auto q = MdRegistry::Get().Raw().query().all<Building>().all<WorldTransform>();
    return q;
}
#else
flecs::query<Building, WorldTransform>& BuildingWorldTransform() {
    static auto q = MdRegistry::Get().Raw().query<Building, WorldTransform>();
    return q;
}
#endif

// Consumers: ai_goal_needs.cpp, ai_goal_vendor.cpp -- both
// const StatSheet&, const WorldTransform& (verified both call sites).
#if defined(MD_ECS_GAIA)
gaia::ecs::Query& StatSheetWorldTransform() {
    static auto q = MdRegistry::Get().Raw().query().all<StatSheet>().all<WorldTransform>();
    return q;
}
#else
flecs::query<StatSheet, WorldTransform>& StatSheetWorldTransform() {
    static auto q = MdRegistry::Get().Raw().query<StatSheet, WorldTransform>();
    return q;
}
#endif

// Consumer: ai_goal_vendor.cpp -- ShopInventory& si, const WorldTransform& tr.
#if defined(MD_ECS_GAIA)
gaia::ecs::Query& ShopInventoryWorldTransform() {
    static auto q = MdRegistry::Get().Raw().query().all<ShopInventory&>().all<WorldTransform>();
    return q;
}
#else
flecs::query<ShopInventory, WorldTransform>& ShopInventoryWorldTransform() {
    static auto q = MdRegistry::Get().Raw().query<ShopInventory, WorldTransform>();
    return q;
}
#endif

void WarmAll() {
    (void)NpcNeedsOnly();
    (void)BleedLimbHealth();
    (void)InjuryLimbHealth();
    (void)AgentStateSchedule();
    (void)AIAgentBT();
    (void)SquadControllers();
    (void)AIAgentBTWorldTransform();
    (void)AgentStateWorldTransform();
    (void)AgentStateOnly();
    (void)SenseWorldTransformAgentState();
    (void)NoiseWorldTransform();
    (void)SenseWorldTransform();
    (void)SmellWorldTransform();
    (void)AgentFullCombat();
    (void)InventoryWorldTransform();
    (void)BuildingWorldTransform();
    (void)StatSheetWorldTransform();
    (void)ShopInventoryWorldTransform();
}

} // namespace ai_queries

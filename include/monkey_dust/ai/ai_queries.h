#pragma once
// ai_queries.h — task #8 Phase 3 (View<T...>() facade removal).
//
// The 18 flecs::query<T...> signatures reachable from the concurrent
// TickNeedsAndInjuries/TickAI JobGraph wave (full manual audit, see
// CLAUDE_STATE.md §7 and the WarmupConcurrentQueries() history this file
// replaces — 17 signatures were found unwarmed there; NpcNeedsOnly is an
// 18th, added in Phase 3 once TickNeedsAndInjuries's bare View<NpcNeeds>()
// call site was traced to the same shared cache NeedsAiSnapshot::Gather()
// relies on already being warm). These need TWO properties the old
// MdRegistry::View<T...>() facade gave for free via its header-template
// function-local static, and which a plain per-call-site
// `static auto q = reg.Raw().query<T...>();` at each of the many call
// sites would NOT give:
//
//   1. SHARED per signature — every call site for the same T... pack
//      must hit the SAME flecs::query object, not a private copy, or the
//      "warm it once before the wave starts" strategy below doesn't
//      actually warm the copy a concurrent caller uses.
//   2. SINGLE-THREADED FIRST CONSTRUCTION — flecs::world::query<T...>()
//      registers with the live world; calling it for the first time from
//      two JobGraph worker threads at once (TickNeedsAndInjuries and
//      TickAI both touching an unwarmed signature on their first tick)
//      previously caused this exact race, fixed by warming all of these
//      up front from a single thread (see logic_tick.cpp's
//      WarmupConcurrentQueries()).
//
// Named, non-template functions (defined once in ai_queries.cpp, declared
// here) give both properties without the header-template's per-.so
// duplication risk: ai_goal_needs.cpp/ai_goal_vendor.cpp are compiled both
// into the main game binary AND into the hot-reloadable libgameplay.so
// (game/CMakeLists.txt's MONKEY_DUST_HOT_RELOAD target) — the .so does
// not link the engine statically, so a template instantiation living in a
// header would get an independent, unwarmed copy on the .so side. A real
// exported function (defined in engine/src/ai/ai_queries.cpp, compiled
// into monkey_dust_engine and therefore into the host monkey_dust/
// monkey_dust_editor binaries) resolves to the SAME already-warmed
// definition at dlopen time instead (the host is built with
// --export-dynamic, the .so with -Wl,--allow-shlib-undefined, the same
// mechanism every other engine symbol the .so calls already relies on).
//
// Lives in engine/ rather than game/ because sense_system.h and
// squad_controller.h (both engine/ headers, engine/ may not #include
// game/) also need some of these signatures — game/ code may include
// engine/ headers, not the reverse (split-readiness rule, CLAUDE.md).
//
// Every OTHER View<T...>() call site in the codebase (outside this
// 18-signature set) is not reachable from the concurrent wave and uses a
// plain per-call-site static query — no need for this shared-registry
// treatment there.

#include <monkey_dust/ecs/md_registry.h>
#include <monkey_dust/world/world_transform.h>
#include <monkey_dust/components/ai_agent.h>
#include <monkey_dust/components/bt_component.h>
#include <monkey_dust/components/agent_state.h>
#include <monkey_dust/components/schedule_component.h>
#include <monkey_dust/components/sense_component.h>
#include <monkey_dust/components/combat.h>
#include <monkey_dust/components/noise_emitter.h>
#include <monkey_dust/components/inventory.h>
#include <monkey_dust/components/building.h>
#include <monkey_dust/components/stat_sheet.h>
#include <monkey_dust/world/shop_inventory.h>
#include <monkey_dust/components/bleed_component.h>
#include <monkey_dust/components/health.h>
#include <monkey_dust/components/injury_state.h>
#include <monkey_dust/components/npc_needs.h>

// Forward-declared, not #included: squad_controller.h itself needs to call
// ai_queries::SquadControllers() (its SquadSystem::Update() is inline in
// the header), which would make squad_controller.h <-> ai_queries.h a real
// circular #include if this header pulled in the full definition — a
// declaration only needs SquadController as an incomplete type (it's used
// here purely as a flecs::query<SquadController>& return type). The
// definition of SquadControllers() in ai_queries.cpp #includes
// squad_controller.h directly for the complete type it actually needs.
struct SquadController;

namespace ai_queries {

// Bare NpcNeeds is used both by NeedsAiSnapshot::Gather() (always serial,
// before the wave) and directly inside TickNeedsAndInjuries (concurrent) —
// needs to be the SAME shared object so Gather()'s early construction
// actually covers the concurrent call site too. The other 3 single-
// component signatures Gather() touches (LimbHealth/Health, BleedComponent,
// InjuryState) are Gather()-only — no other concurrent-tree call site uses
// their bare (non-combined) form, so those stay plain per-call-site statics.
// gaia's Query is a single non-templated type (using Query =
// detail::QueryImpl, docs/GAIA_SEAM_AUDIT.md) -- unlike flecs::query<T...>,
// it carries no compile-time component list, so every one of these 18
// signatures collapses to the SAME return type under MD_ECS_GAIA. Which
// components are mutable vs read-only is baked into each function's gaia
// query BUILD (via .all<T&>() vs .all<T>()) in ai_queries.cpp instead of
// being visible in the return type here -- determined by auditing every
// real MdEach consumer of each function across the codebase (not guessed),
// see ai_queries.cpp's per-function comments for the consumer list.
gaia::ecs::Query& NpcNeedsOnly();
gaia::ecs::Query& BleedLimbHealth();
gaia::ecs::Query& InjuryLimbHealth();
gaia::ecs::Query& AgentStateSchedule();
gaia::ecs::Query& AIAgentBT();
gaia::ecs::Query& SquadControllers();
gaia::ecs::Query& AIAgentBTWorldTransform();
gaia::ecs::Query& AgentStateWorldTransform();
gaia::ecs::Query& AgentStateOnly();
gaia::ecs::Query& SenseWorldTransformAgentState();
gaia::ecs::Query& NoiseWorldTransform();
gaia::ecs::Query& SenseWorldTransform();
gaia::ecs::Query& SmellWorldTransform();
gaia::ecs::Query& AgentFullCombat();
gaia::ecs::Query& InventoryWorldTransform();
gaia::ecs::Query& BuildingWorldTransform();
gaia::ecs::Query& StatSheetWorldTransform();
gaia::ecs::Query& ShopInventoryWorldTransform();

// Forces construction of all 18 above, single-threaded — call once at
// startup, before the first JobGraph wave that runs TickNeedsAndInjuries
// and TickAI concurrently (see logic_tick.cpp's WarmupConcurrentQueries()).
void WarmAll();

} // namespace ai_queries

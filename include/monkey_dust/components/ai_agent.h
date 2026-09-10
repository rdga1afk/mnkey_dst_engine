#pragma once
#include <monkey_dust/ecs/md_entity.h>
#include <cstdint>
#if defined(MD_ECS_GAIA)
#include <gaia.h>
#endif

// GAIA_STORAGE(Sparse) -- Phase 2 (prompt_/PROMPT_GAIA_MIGRATION.md §0/§4):
// one of the 5 components with a proven B3.4 risk. See world_transform.h's
// doc comment for the shared rationale.
struct AIAgent {
#if defined(MD_ECS_GAIA)
    GAIA_STORAGE(Sparse);
#endif
    uint32_t     faction_id;
    uint8_t      bt_template_id    = 0;  // 255 = schedule NPC
    uint8_t      level             = 1;  // NPC power level [1..255]; scales hp and damage
};

// audit S1-00 (2026-08-27, flecs abort on first AI tick): every field that
// changes more than once at spawn used to live on AIAgent itself --
// AISystem::Update wrote last_tick_ms/bt_node every tick, FlushTransformSoA
// (logic_tick_orchestration.cpp) writes lod_level every tick BEFORE the
// JobGraph wave even starts, and combat_system.cpp writes last_attacker/
// personal_relation on hit. flecs's dirty/change monitor is per-component-
// per-table, not per-field, so ANY write to ANY AIAgent field (even one
// CompareAIAgentFaction never reads) marks the WHOLE component dirty,
// forcing ai_queries::AIAgentBTWorldTransform()'s .order_by<AIAgent>
// (CompareAIAgentFaction) to resort on its next iteration. That resort
// unconditionally asserts if the world is flagged EcsWorldMultiThreaded at
// the time (flecs.c's own documented API contract: sorted-query cache
// rebuilds are unsupported in multithreaded mode) -- and JobGraph::Run()
// sets exactly that flag whenever the "AI" batch shares a wave with
// another (the designed common case for AI + NeedsAndInjuries, not an
// edge case). A first attempt at this fix moved only last_tick_ms/bt_node
// out and still crashed, because FlushTransformSoA's lod_level write
// (single-threaded, but happening every tick immediately before
// RunTickSystemsViaJobGraph flips the world multithreaded) was already
// enough on its own to dirty AIAgent before the sorted query's very next
// iteration -- moving every per-tick/on-hit-mutated field out, not just
// the ones AISystem::Update itself touches, is what actually closes this.
// AIAgent is now genuinely write-once-at-spawn (faction_id/bt_template_id/
// level all set at emplace time and left alone) -- order_by<AIAgent> can
// never see it dirty again after the entity's initial creation.
struct AIAgentTickState {
    float last_tick_ms         = 0.f;
    int8_t bt_node             = -1;
    uint8_t lod_level          = 0;
    int8_t personal_relation   = 0;
    MdEntity last_attacker     = MdEntity::Null();
};

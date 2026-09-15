#include <monkey_dust/ai/behavior_tree.h>
#include <monkey_dust/ecs/md_registry.h>
#include <monkey_dust/components/agent_state.h>
#include <monkey_dust/components/bt_components.h>
#include <monkey_dust/components/sense_component.h>
#include <monkey_dust/components/npc_memory.h>
#include <monkey_dust/ai/squad_signal.h>
#include <monkey_dust/ai/suspicious_item_group.h>
#include <monkey_dust/ai/director_system.h>
#include <monkey_dust/ai/alien_config.h>
#include <monkey_dust/ai/vent_lock.h>
#include <monkey_dust/ai/vent_registry.h>
#include <monkey_dust/nav/path_cache.h>
#include <monkey_dust/components/weapon_component.h>
#include <monkey_dust/components/nav_agent.h>
#include <monkey_dust/world/world_transform.h>
#include <monkey_dust/world/alliance.h>
#include <monkey_dust/ai/npc_development.h>
#include <monkey_dust/ai/fnv.h>
#include <monkey_dust/ai/bt_lua_script_registry.h>
#include <monkey_dust/components/lua_script_component.h>
#include <monkey_dust/scripting/lua_system.h>
#include <cstring>
#include <cmath>
#include <monkey_dust/math/md_fast_math.h>
#include <monkey_dust/ai/token_registry.h>

static constexpr uint32_t TARGET_ENTITY_BB_KEY            = md::fnv1a("target_entity");
static constexpr uint32_t SQUAD_TX_BB_KEY                 = md::fnv1a("squad_tx");
static constexpr uint32_t SQUAD_TZ_BB_KEY                 = md::fnv1a("squad_tz");
static constexpr uint32_t SQUAD_ACTIVITY_BB_KEY           = md::fnv1a("squad_activity");
static constexpr uint32_t NEXT_TARGET_ENTITY_BB_KEY       = md::fnv1a("next_target_entity");
static constexpr uint32_t HAS_SEARCHED_POS_BB_KEY         = md::fnv1a("has_searched_pos");
static constexpr uint32_t DONE_SUSPECT_MOVETO_BB_KEY      = md::fnv1a("done_suspect_moveto");
static constexpr uint32_t DONE_SYSTEMATIC_SEARCH_BB_KEY   = md::fnv1a("done_systematic_search");
static constexpr uint32_t HAS_OBJECTIVE_BB_KEY            = md::fnv1a("has_objective");

// ── Stackless VM tick ─────────────────────────────────────────────────────────
BTStatus BehaviorTree::tick(md::EngineContext& ctx, MdEntity e, AgentState* as, uint32_t nowMs) {
    if (!isValid()) return BTStatus::Failure;

    uint16_t pc     = m_root;
    BTStatus result = BTStatus::Running;

    static constexpr int VM_MAX_STEPS = 4096;
    int vm_steps = 0;

    while (true) {
        if (++vm_steps > VM_MAX_STEPS) break;
        if (pc == INVALID) break;

        BTNode&  nd = m_nodes[pc];
        BTState& st = m_state[pc];

        switch (nd.type) {
#include "bt_vm_core.inc"
#include "bt_vm_ai.inc"
#include "bt_vm_ext.inc"
        default:
            goto exit_loop;
        }
    }

exit_loop:
    return result;
}

#include <monkey_dust/ecs/component_warmup.h>
#include <monkey_dust/ecs/registry.h>
#include <monkey_dust/ecs/md_registry.h>
#include <monkey_dust/ecs/component_reflect.h>
#include <monkey_dust/platform/md_log.h>

#include <monkey_dust/components/agent_state.h>
#include <monkey_dust/components/ai_agent.h>
#include <monkey_dust/components/ai_script.h>
#include <monkey_dust/components/animator.h>
#include <monkey_dust/components/bleed_component.h>
#include <monkey_dust/components/bounty_component.h>
#include <monkey_dust/components/bt_component.h>
#include <monkey_dust/components/bt_components.h>
#include <monkey_dust/components/building.h>
#include <monkey_dust/components/char_body_state.h>
#include <monkey_dust/components/combat.h>
#include <monkey_dust/components/equipment.h>
#include <monkey_dust/components/faction.h>
#include <monkey_dust/components/flare_actor.h>
#include <monkey_dust/components/flare_sprite_anim.h>
#include <monkey_dust/components/health.h>
#include <monkey_dust/components/hierarchy.h>
#include <monkey_dust/components/injury_state.h>
#include <monkey_dust/components/inventory.h>
#include <monkey_dust/components/lock_component.h>
#include <monkey_dust/components/lua_script_component.h>
#include <monkey_dust/components/nav_agent.h>
#include <monkey_dust/components/noise_emitter.h>
#include <monkey_dust/components/npc_memory.h>
#include <monkey_dust/components/npc_needs.h>
#include <monkey_dust/components/npc_relationship.h>
#include <monkey_dust/components/player_controller.h>
#include <monkey_dust/components/prisoner_component.h>
#include <monkey_dust/components/projectile.h>
#include <monkey_dust/components/renderable.h>
#include <monkey_dust/components/sense_component.h>
#include <monkey_dust/components/skill_xp_accum.h>
#include <monkey_dust/components/stat_sheet.h>
#include <monkey_dust/components/stealth_component.h>
#include <monkey_dust/components/weapon_component.h>

#include <monkey_dust/combat/damage_calc.h>
#include <monkey_dust/combat/impact_event.h>
#include <monkey_dust/combat/limb_severance.h>

#include <monkey_dust/ai/npc_development.h>
#include <monkey_dust/ai/patrol_route.h>
#include <monkey_dust/ai/squad_controller.h>
#include <monkey_dust/ai/squad_signal.h>
#include <monkey_dust/ai/suspicious_item_group.h>

#include <monkey_dust/physics/jolt_world.h>
#include <monkey_dust/physics/ragdoll.h>

#include <monkey_dust/scripting/flow_graph.h>

#include <monkey_dust/world/alliance.h>
#include <monkey_dust/world/interior_portal.h>
#include <monkey_dust/world/shop_inventory.h>
#include <monkey_dust/world/world_transform.h>

#include <monkey_dust/building/building_integrity.h>

#include <cctype>
#include <cstdio>

namespace md {

// Point 4 (concurrency audit): deliberately NOT gated behind #ifndef NDEBUG —
// this project's own documented default build is
// `-DCMAKE_BUILD_TYPE=Release` (confirmed via build/CMakeCache.txt), so an
// NDEBUG-gated startup check would never run in the configuration actually
// used day to day. This runs once at startup; cost is negligible.
void VerifyReflectedComponentsAreWarmedUp();

void WarmUpEngineComponents() {
    auto& w = Registry::Get();

    w.component<AgentBlackboard>();
    w.component<AgentState>();
    w.component<AIAgent>();
    w.component<AIAgentTickState>();
    w.component<AIScript>();
    w.component<AnimatorComponent>();
    w.component<BehaviorTreeComponent>();
    w.component<BleedComponent>();
    w.component<BodyBaseline>();
    w.component<BountyComponent>();
    w.component<BTComponent>();
    w.component<Building>();
    w.component<CharBodyState>();
    w.component<CombatModifiers>();
    w.component<DetachedLimb>();
    w.component<DirectorHintComponent>();
    w.component<EquipmentComponent>();
    w.component<Faction>();
    // Fear/Trust (npc_relationship.h) and FriendlyWith/HostileWith
    // (alliance.h) are private nested relation-tag types inside their
    // owning classes — can't be touched from here. If either ever gets
    // used for the first time from inside a JobGraph batch, that class
    // needs its own warm-up method (e.g. AllianceMatrix::WarmUp()) called
    // from here instead.
    w.component<FlareActorComponent>();
    w.component<FlareSpriteAnim>();
    w.component<FlowGraph>();
    w.component<Health>();
    w.component<ImpactEvent>();
    w.component<InjuryState>();
    w.component<InteriorPortal>();
    w.component<Inventory>();
    w.component<LockComponent>();
    w.component<LuaScriptComponent>();
    w.component<MdManagedTag>();
    w.component<NavAgent>();
    w.component<NoiseEmitter>();
    w.component<SmellEmitter>();
    w.component<NpcDevelopmentComponent>();
    w.component<NpcMemoryComponent>();
    w.component<NpcNeeds>();
    w.component<NpcRelationshipComponent>();
    w.component<ParentRef>();
    w.component<ChildrenRef>();
    w.component<PatrolRoute>();
    w.component<PhysicsAgent>();
    w.component<PlayerController>();
    w.component<PrisonerComponent>();
    w.component<ProjectileComponent>();
    w.component<RagdollComponent>();
    w.component<Renderable>();
    w.component<SenseComponent>();
    w.component<SenseModifiers>();
    w.component<ShopInventory>();
    w.component<SkillXpAccum>();
    w.component<SquadController>();
    w.component<SquadMemberComponent>();
    w.component<StatSheet>();
    w.component<StealthComponent>();
    w.component<SuspiciousItemGroupComponent>();
    w.component<WeaponComponent>();
    w.component<CollapseState>();

    // Phase 5.1 (audit) proof-of-concept: WorldTransform/LimbHealth/Combat
    // are the 3 components this master list currently covers — see
    // component_master_list.h's doc comment for scope/rationale. Every
    // OTHER component above still has its own explicit w.component<T>()
    // line; this isn't a wholesale migration, just a working demonstration
    // that the pattern is safe (same generated calls, same behavior).
#define MD_COMPONENT(CppType) w.component<CppType>();
#include <monkey_dust/ecs/component_master_list.h>

    fprintf(stdout, "[ComponentWarmup] all engine ECS component types registered\n");

    // gaia-ecs migration (prompt_/PROMPT_GAIA_MIGRATION.md §3 p.5):
    // sizeof(T) < 8191 is gaia's real, code-enforced component-size limit
    // (docs/GAIA_MIGRATION_ANALYSIS.md §0 -- id.h:46,48, NOT the README's
    // stale "4095 bytes"). A standing check, not a one-off script, so a
    // future component that grows past the limit fails the build under
    // MD_ECS_GAIA=ON immediately rather than asserting at runtime deep
    // inside gaia's own component-cache code. Largest known today:
    // BTComponent=4648, FlowGraph=4384 -- both comfortably under.
#if defined(MD_ECS_GAIA)
    static_assert(sizeof(AgentBlackboard) < 8191);
    static_assert(sizeof(AgentState) < 8191);
    static_assert(sizeof(AIAgent) < 8191);
    static_assert(sizeof(AIAgentTickState) < 8191);
    static_assert(sizeof(AIScript) < 8191);
    static_assert(sizeof(AnimatorComponent) < 8191);
    static_assert(sizeof(BehaviorTreeComponent) < 8191);
    static_assert(sizeof(BleedComponent) < 8191);
    static_assert(sizeof(BodyBaseline) < 8191);
    static_assert(sizeof(BountyComponent) < 8191);
    static_assert(sizeof(BTComponent) < 8191);
    static_assert(sizeof(Building) < 8191);
    static_assert(sizeof(CharBodyState) < 8191);
    static_assert(sizeof(ChildrenRef) < 8191);
    static_assert(sizeof(CollapseState) < 8191);
    static_assert(sizeof(CombatModifiers) < 8191);
    static_assert(sizeof(DetachedLimb) < 8191);
    static_assert(sizeof(DirectorHintComponent) < 8191);
    static_assert(sizeof(EquipmentComponent) < 8191);
    static_assert(sizeof(Faction) < 8191);
    static_assert(sizeof(FlareActorComponent) < 8191);
    static_assert(sizeof(FlareSpriteAnim) < 8191);
    static_assert(sizeof(FlowGraph) < 8191);
    static_assert(sizeof(Health) < 8191);
    static_assert(sizeof(ImpactEvent) < 8191);
    static_assert(sizeof(InjuryState) < 8191);
    static_assert(sizeof(InteriorPortal) < 8191);
    static_assert(sizeof(Inventory) < 8191);
    static_assert(sizeof(LockComponent) < 8191);
    static_assert(sizeof(LuaScriptComponent) < 8191);
    static_assert(sizeof(MdManagedTag) < 8191);
    static_assert(sizeof(NavAgent) < 8191);
    static_assert(sizeof(NoiseEmitter) < 8191);
    static_assert(sizeof(NpcDevelopmentComponent) < 8191);
    static_assert(sizeof(NpcMemoryComponent) < 8191);
    static_assert(sizeof(NpcNeeds) < 8191);
    static_assert(sizeof(NpcRelationshipComponent) < 8191);
    static_assert(sizeof(ParentRef) < 8191);
    static_assert(sizeof(PatrolRoute) < 8191);
    static_assert(sizeof(PhysicsAgent) < 8191);
    static_assert(sizeof(PlayerController) < 8191);
    static_assert(sizeof(PrisonerComponent) < 8191);
    static_assert(sizeof(ProjectileComponent) < 8191);
    static_assert(sizeof(RagdollComponent) < 8191);
    static_assert(sizeof(Renderable) < 8191);
    static_assert(sizeof(SenseComponent) < 8191);
    static_assert(sizeof(SenseModifiers) < 8191);
    static_assert(sizeof(ShopInventory) < 8191);
    static_assert(sizeof(SkillXpAccum) < 8191);
    static_assert(sizeof(SmellEmitter) < 8191);
    static_assert(sizeof(SquadController) < 8191);
    static_assert(sizeof(SquadMemberComponent) < 8191);
    static_assert(sizeof(StatSheet) < 8191);
    static_assert(sizeof(StealthComponent) < 8191);
    static_assert(sizeof(SuspiciousItemGroupComponent) < 8191);
    static_assert(sizeof(WeaponComponent) < 8191);
    static_assert(sizeof(WorldTransform) < 8191);
    // Not included here: Combat, LimbHealth -- registered via the
    // component_master_list.h X-macro above (MD_COMPONENT) rather than a
    // w.component<T>() call visible in this file's own grep-verified list
    // (docs/GAIA_SEAM_AUDIT.md §1), and this TU does not visibly include
    // a dedicated header defining them -- add once confirmed which header
    // actually declares them, rather than guess an include path here.
#endif

    VerifyReflectedComponentsAreWarmedUp();
}

namespace {
// snake_case -> PascalCase, matching flecs's auto-derived (RTTI-based)
// component names — same conversion rule as
// tools/editor/editor_reflect_bridge.h's EcsReflectBridge::ToPascalCase
// (duplicated here rather than shared: that file is editor-only, guarded by
// MONKEY_DUST_EDITOR, and deliberately stays on the untyped flecs C API to
// avoid a vague-linkage risk across a dlopen boundary that doesn't apply to
// this always-linked engine .cpp).
void ToPascalCase(const char* snake, char* out, int out_size) {
    int o = 0;
    char buf[64];
    int bi = 0;
    auto flush_segment = [&]() {
        if (bi == 0) return;
        buf[bi] = '\0';
        if (bi == 2 && (buf[0] == 'a' || buf[0] == 'A') && (buf[1] == 'i' || buf[1] == 'I')) {
            if (o < out_size - 2) { out[o++] = 'A'; out[o++] = 'I'; }
        } else {
            for (int k = 0; k < bi && o < out_size - 1; ++k)
                out[o++] = (k == 0) ? (char)toupper((unsigned char)buf[k]) : buf[k];
        }
        bi = 0;
    };
    for (int i = 0; ; ++i) {
        char c = snake[i];
        if (c == '_' || c == '\0') {
            flush_segment();
            if (c == '\0') break;
            continue;
        }
        if (bi < (int)sizeof(buf) - 1) buf[bi++] = c;
    }
    out[o] = '\0';
}
}  // namespace

// Point 4 (concurrency audit): WarmUpEngineComponents() (55 flecs type
// registrations, this file) and RegisterCoreComponents() (10 editor-Inspector
// field-reflection entries, component_reflect.cpp) serve different purposes
// and are NOT expected to have matching counts — RegisterCoreComponents() is
// a deliberately narrow, hand-picked subset (its own source comments say
// "skip X — large array", "skip Y — complex nested struct"). The invariant
// worth guarding is one-directional: every component RegisterCoreComponents()
// reflects must ALSO be flecs-registered here, since the editor Inspector
// can't safely reflect a type flecs has never seen. Uses ecs_lookup() (a
// read-only lookup, does NOT register) — same name-resolution approach as
// tools/editor/editor_reflect_bridge.h's EcsReflectBridge::Init(), reused
// here as a startup consistency check instead of a live editor bridge.
void VerifyReflectedComponentsAreWarmedUp() {
    auto& w = Registry::Get();
    const ComponentReflect& reg = ComponentReflect::Get();
    int n = reg.Count();
    for (int i = 0; i < n; ++i) {
        const ComponentDesc& desc = reg.GetDesc(i);
        char pascal[40];
        ToPascalCase(desc.name, pascal, sizeof(pascal));
        ecs_entity_t id = ecs_lookup(w.c_ptr(), pascal);
        if (id == 0) {
            MD_LOG(MD_LOG_ERROR,
                   "[ComponentWarmup] '%s' (reflected as '%s' in "
                   "RegisterCoreComponents) is not flecs-registered — add "
                   "w.component<%s>() to WarmUpEngineComponents().",
                   desc.name, pascal, pascal);
        }
    }
}

}  // namespace md

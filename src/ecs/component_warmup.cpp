#include <monkey_dust/ecs/component_warmup.h>
#include <monkey_dust/ecs/registry.h>
#include <monkey_dust/ecs/md_registry.h>
#include <monkey_dust/ecs/component_reflect.h>
#include <monkey_dust/platform/md_log.h>

#include <monkey_dust/components/animator.h>
#include <monkey_dust/components/char_body_state.h>
#include <monkey_dust/components/health.h>
#include <monkey_dust/components/hierarchy.h>
#include <monkey_dust/components/nav_agent.h>
#include <monkey_dust/components/player_controller.h>
#include <monkey_dust/components/renderable.h>

#include <monkey_dust/physics/jolt_world.h>
#include <monkey_dust/physics/ragdoll.h>

#include <monkey_dust/world/world_transform.h>

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

    w.add<AnimatorComponent>();
    w.add<BodyBaseline>();
    w.add<CharBodyState>();
    w.add<Health>();
    w.add<MdManagedTag>();
    w.add<NavAgent>();
    w.add<ParentRef>();
    w.add<ChildrenRef>();
    w.add<PhysicsAgent>();
    w.add<PlayerController>();
    w.add<RagdollComponent>();
    w.add<Renderable>();

    // Phase 5.1 (audit) proof-of-concept: WorldTransform/LimbHealth/Combat
    // are the 3 components this master list currently covers — see
    // component_master_list.h's doc comment for scope/rationale. Every
    // OTHER component above still has its own explicit w.component<T>()
    // line; this isn't a wholesale migration, just a working demonstration
    // that the pattern is safe (same generated calls, same behavior).
#define MD_COMPONENT(CppType) w.add<CppType>();
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
    static_assert(sizeof(AnimatorComponent) < 8191);
    static_assert(sizeof(BodyBaseline) < 8191);
    static_assert(sizeof(CharBodyState) < 8191);
    static_assert(sizeof(ChildrenRef) < 8191);
    static_assert(sizeof(Health) < 8191);
    static_assert(sizeof(MdManagedTag) < 8191);
    static_assert(sizeof(NavAgent) < 8191);
    static_assert(sizeof(ParentRef) < 8191);
    static_assert(sizeof(PhysicsAgent) < 8191);
    static_assert(sizeof(PlayerController) < 8191);
    static_assert(sizeof(RagdollComponent) < 8191);
    static_assert(sizeof(Renderable) < 8191);
    static_assert(sizeof(WorldTransform) < 8191);

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
// gaia-ecs migration (Phase 5, PROMPT_GAIA_MIGRATION.md §7 p.2-3): gaia's
// World::resolve(name) is the direct ecs_lookup() equivalent -- read-only,
// does not register, returns EntityBad on no match (verified via a
// standalone probe against an UNNAMESPACED struct, matching this
// codebase's real component declaration style: gaia's RTTI-derived auto
// name for such a type is the bare unqualified name, e.g. "AgentState",
// identical to what ToPascalCase(reflect_name) below already produces for
// the flecs branch -- no separate naming convention to reconcile).
void VerifyReflectedComponentsAreWarmedUp() {
    auto& w = Registry::Get();
    const ComponentReflect& reg = ComponentReflect::Get();
    int n = reg.Count();
    for (int i = 0; i < n; ++i) {
        const ComponentDesc& desc = reg.GetDesc(i);
        char pascal[40];
        ToPascalCase(desc.name, pascal, sizeof(pascal));
        gaia::ecs::Entity id = w.resolve(pascal);
        if (id == gaia::ecs::EntityBad) {
            MD_LOG(MD_LOG_ERROR,
                   "[ComponentWarmup] '%s' (reflected as '%s' in "
                   "RegisterCoreComponents) is not gaia-registered — add "
                   "w.add<%s>() to WarmUpEngineComponents().",
                   desc.name, pascal, pascal);
        }
    }
}

}  // namespace md

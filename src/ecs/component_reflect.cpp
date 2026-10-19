#include <monkey_dust/ecs/component_reflect.h>
#include <monkey_dust/world/world_transform.h>
#include <monkey_dust/components/health.h>
#include <monkey_dust/components/nav_agent.h>
#include <monkey_dust/components/renderable.h>
#include <cstdio>
#include <cstring>

namespace md {

// ── FNV-1a 32-bit ─────────────────────────────────────────────────────────────
uint32_t ComponentReflect::Hash(const char* s) {
    uint32_t h = 2166136261u;
    for (; *s; ++s) h = (h ^ (uint8_t)*s) * 16777619u;
    return h ? h : 1u;
}

// ── Singleton ─────────────────────────────────────────────────────────────────
ComponentReflect& ComponentReflect::Get() {
    static ComponentReflect inst;
    return inst;
}

// ── Register ──────────────────────────────────────────────────────────────────
bool ComponentReflect::Register(const char* name,
                                 const FieldDesc* fields, int field_count,
                                 uint16_t component_size) {
    if (!name || !fields || field_count <= 0) return false;
    if (count_ >= MAX_COMPONENTS) {
        fprintf(stderr, "[ComponentReflect] MAX_COMPONENTS=%d reached, skip '%s'\n",
                MAX_COMPONENTS, name);
        return false;
    }
    uint32_t h = Hash(name);
    for (int i = 0; i < count_; ++i)
        if (descs_[i].name_hash == h) return false;  // already registered

    ComponentDesc& d = descs_[count_++];
    strncpy(d.name, name, sizeof(d.name) - 1);
    d.name_hash      = h;
    d.fields         = fields;
    d.field_count    = (uint8_t)(field_count < 24 ? field_count : 24);
    d.component_size = component_size;
    return true;
}

// ── Find ──────────────────────────────────────────────────────────────────────
const ComponentDesc* ComponentReflect::Find(uint32_t h) const {
    for (int i = 0; i < count_; ++i)
        if (descs_[i].name_hash == h) return &descs_[i];
    return nullptr;
}

const ComponentDesc* ComponentReflect::Find(const char* name) const {
    return Find(Hash(name));
}



// ── RegisterCoreComponents ────────────────────────────────────────────────────
// Reflects engine-owned components. Call once at startup after ECS init.

void RegisterCoreComponents() {
    auto& r = ComponentReflect::Get();

    // WorldTransform — spatial position + rotation (top-down RPG)
    static const FieldDesc wt_fields[] = {
        MD_FIELD(WorldTransform, x,     F32),
        MD_FIELD(WorldTransform, y,     F32),
        MD_FIELD(WorldTransform, z,     F32),
        MD_FIELD_RANGE(WorldTransform, rot_y, F32, 1.f, -180.f, 180.f),
        MD_FIELD(WorldTransform, slot,  U32),
    };
    r.Register("world_transform", wt_fields,
               (int)(sizeof(wt_fields) / sizeof(wt_fields[0])),
               (uint16_t)sizeof(WorldTransform));

    // LimbHealth — per-limb hp/max; skip flags[] (bitmask array).
    // NOTE: reflection name is "limb_health" (not "health") — `Health` is a
    // `using Health = LimbHealth;` alias (health.h), and flecs derives a
    // component's auto-registered name from the real (post-alias) type via
    // RTTI, so the entity EcsReflectBridge must resolve is "LimbHealth".
    static const FieldDesc health_fields[] = {
        MD_FIELD_RANGE(LimbHealth, hp[0], F32, 1.f, 0.f, 500.f),
        MD_FIELD_RANGE(LimbHealth, hp[1], F32, 1.f, 0.f, 500.f),
        MD_FIELD_RANGE(LimbHealth, hp[2], F32, 1.f, 0.f, 500.f),
        MD_FIELD_RANGE(LimbHealth, hp[3], F32, 1.f, 0.f, 500.f),
        MD_FIELD_RANGE(LimbHealth, hp[4], F32, 1.f, 0.f, 500.f),
        MD_FIELD_RANGE(LimbHealth, hp[5], F32, 1.f, 0.f, 500.f),
        MD_FIELD_RANGE(LimbHealth, max[0], F32, 1.f, 0.f, 500.f),
        MD_FIELD_RANGE(LimbHealth, max[1], F32, 1.f, 0.f, 500.f),
        MD_FIELD_RANGE(LimbHealth, max[2], F32, 1.f, 0.f, 500.f),
        MD_FIELD_RANGE(LimbHealth, max[3], F32, 1.f, 0.f, 500.f),
        MD_FIELD_RANGE(LimbHealth, max[4], F32, 1.f, 0.f, 500.f),
        MD_FIELD_RANGE(LimbHealth, max[5], F32, 1.f, 0.f, 500.f),
        MD_FIELD(LimbHealth, incapacitated, Bool),
    };
    r.Register("limb_health", health_fields,
               (int)(sizeof(health_fields) / sizeof(health_fields[0])),
               (uint16_t)sizeof(LimbHealth));

    // NavAgent — skip render_x/z (internal smoothing).
    static const FieldDesc nav_agent_fields[] = {
        MD_FIELD(NavAgent, target_x, F32),
        MD_FIELD(NavAgent, target_z, F32),
        MD_FIELD(NavAgent, is_moving, Bool),
        MD_FIELD_RANGE(NavAgent, move_speed, F32, 0.1f, 0.f, 10.f),
        MD_FIELD_RANGE(NavAgent, walk_speed, F32, 0.1f, 0.f, 20.f),
        MD_FIELD_RANGE(NavAgent, run_speed,  F32, 0.1f, 0.f, 20.f),
        MD_FIELD(NavAgent, desired_vel_x, F32),
        MD_FIELD(NavAgent, desired_vel_z, F32),
    };
    r.Register("nav_agent", nav_agent_fields,
               (int)(sizeof(nav_agent_fields) / sizeof(nav_agent_fields[0])),
               (uint16_t)sizeof(NavAgent));

    // Renderable
    static const FieldDesc renderable_fields[] = {
        MD_FIELD(Renderable, sprite_id, U8),
        MD_FIELD_RANGE(Renderable, lod_level, U8, 1.f, 0.f, 2.f),
        MD_FIELD(Renderable, visible, Bool),
    };
    r.Register("renderable", renderable_fields,
               (int)(sizeof(renderable_fields) / sizeof(renderable_fields[0])),
               (uint16_t)sizeof(Renderable));

    fprintf(stdout, "[ComponentReflect] %d components registered\n", r.Count());
}

} // namespace md

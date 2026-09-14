#include <monkey_dust/ecs/component_reflect.h>
#include <monkey_dust/world/world_transform.h>
#include <monkey_dust/components/sense_component.h>
#include <monkey_dust/components/health.h>
#include <monkey_dust/components/combat.h>
#include <monkey_dust/components/ai_agent.h>
#include <monkey_dust/components/nav_agent.h>
#include <monkey_dust/components/renderable.h>
#include <monkey_dust/components/building.h>
#include <monkey_dust/components/inventory.h>
#include <monkey_dust/components/stat_sheet.h>
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

// ── Dump ──────────────────────────────────────────────────────────────────────
void ComponentReflect::Dump(const void* ptr, const ComponentDesc& d) {
    fprintf(stdout, "[%s] (%u bytes)\n", d.name, d.component_size);
    for (int i = 0; i < d.field_count; ++i) {
        const FieldDesc& f = d.fields[i];
        const uint8_t*   p = (const uint8_t*)ptr + f.offset;
        fprintf(stdout, "  %-20s = ", f.name);
        // memcpy, not a reinterpret cast through `p` -- debug-only path
        // (called rarely, never per-frame), so the copy is free; avoids
        // cppcheck's real dangerousTypeCast/invalidPointerCast concern
        // (strict-aliasing UB reading a float/int through a uint8_t*)
        // for zero behavior change.
        switch (f.type) {
        case FieldType::F32:   { float v;    memcpy(&v, p, sizeof(v)); fprintf(stdout, "%.4f", v); break; }
        case FieldType::I32:   { int32_t v;  memcpy(&v, p, sizeof(v)); fprintf(stdout, "%d",   v); break; }
        case FieldType::U32:   { uint32_t v; memcpy(&v, p, sizeof(v)); fprintf(stdout, "%u",   v); break; }
        case FieldType::U8:    fprintf(stdout, "%u",     (unsigned)*p);        break;
        case FieldType::U16:   { uint16_t v; memcpy(&v, p, sizeof(v)); fprintf(stdout, "%u",   v); break; }
        case FieldType::U64:   { uint64_t v; memcpy(&v, p, sizeof(v)); fprintf(stdout, "%llu", (unsigned long long)v); break; }
        case FieldType::Bool:  fprintf(stdout, "%s",     *p ? "true" : "false"); break;
        case FieldType::Vec3: {
            float v[3];
            memcpy(v, p, sizeof(v));
            fprintf(stdout, "(%.3f, %.3f, %.3f)", v[0], v[1], v[2]);
            break;
        }
        case FieldType::Enum8:  fprintf(stdout, "%u (enum)",  (unsigned)*p); break;
        case FieldType::Enum32: { uint32_t v; memcpy(&v, p, sizeof(v)); fprintf(stdout, "%u (enum)", v); break; }
        default: fprintf(stdout, "?"); break;
        }
        fprintf(stdout, "\n");
    }
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

    // SenseComponent — NPC sense activations (Visual..Background)
    static const FieldDesc sc_fields[] = {
        MD_FIELD(SenseComponent, cone_set_idx,       U8),
        MD_FIELD(SenseComponent, activation[0],      F32),  // Visual
        MD_FIELD(SenseComponent, last_activated_ms[0], U64), // Visual last triggered
        MD_FIELD(SenseComponent, last_known_x,       F32),
        MD_FIELD(SenseComponent, last_known_z,       F32),
    };
    r.Register("sense_component", sc_fields,
               (int)(sizeof(sc_fields) / sizeof(sc_fields[0])),
               (uint16_t)sizeof(SenseComponent));

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

    // Combat — weapon/armor top-level scalars; skip target (MdEntity), rng_state.
    static const FieldDesc combat_fields[] = {
        MD_FIELD_RANGE(Combat, weapon.damage,       F32, 1.f, 0.f, 500.f),
        MD_FIELD(Combat,       weapon.type,          Enum8),
        MD_FIELD_RANGE(Combat, weapon.attack_range,  F32, 0.1f, 0.f, 20.f),
        MD_FIELD(Combat,       weapon.attack_ms,     U32),
        MD_FIELD_RANGE(Combat, armor.blunt_resist,   F32, 0.01f, 0.f, 1.f),
        MD_FIELD_RANGE(Combat, armor.cut_resist,     F32, 0.01f, 0.f, 1.f),
        MD_FIELD_RANGE(Combat, armor.pierce_resist,  F32, 0.01f, 0.f, 1.f),
        MD_FIELD(Combat, is_dead,        Bool),
        MD_FIELD(Combat, last_attack_ms, F32),
        MD_FIELD(Combat, death_timer,    F32),
    };
    r.Register("combat", combat_fields,
               (int)(sizeof(combat_fields) / sizeof(combat_fields[0])),
               (uint16_t)sizeof(Combat));

    // AIAgent — last_tick_ms/bt_node moved to AIAgentTickState (audit S1-00,
    // 2026-08-27): they used to live here, but writing them every tick from
    // inside the order_by<AIAgent> sorted query dirtied AIAgent's own change
    // monitor, forcing a re-sort that flecs asserts on while the world is
    // multithreaded (see ai_agent.h's doc comment for the full mechanism).
    static const FieldDesc ai_agent_fields[] = {
        MD_FIELD(AIAgent, faction_id,        U32),
        MD_FIELD(AIAgent, bt_template_id,    U8),
        MD_FIELD_RANGE(AIAgent, level, U8, 1.f, 1.f, 255.f),
    };
    r.Register("ai_agent", ai_agent_fields,
               (int)(sizeof(ai_agent_fields) / sizeof(ai_agent_fields[0])),
               (uint16_t)sizeof(AIAgent));

    // AIAgentTickState — lod_level/personal_relation moved here too (second
    // S1-00 pass, 2026-08-27): see ai_agent.h's doc comment.
    static const FieldDesc ai_agent_tick_state_fields[] = {
        MD_FIELD(AIAgentTickState, last_tick_ms,      F32),
        MD_FIELD(AIAgentTickState, bt_node,           I8),
        MD_FIELD_RANGE(AIAgentTickState, lod_level, U8, 1.f, 0.f, 2.f),
        MD_FIELD(AIAgentTickState, personal_relation, I8),
    };
    r.Register("ai_agent_tick_state", ai_agent_tick_state_fields,
               (int)(sizeof(ai_agent_tick_state_fields) / sizeof(ai_agent_tick_state_fields[0])),
               (uint16_t)sizeof(AIAgentTickState));

    // NavAgent — skip path[] (large fixed array), render_x/z (internal smoothing).
    static const FieldDesc nav_agent_fields[] = {
        MD_FIELD(NavAgent, target_x, F32),
        MD_FIELD(NavAgent, target_z, F32),
        MD_FIELD(NavAgent, is_moving, Bool),
        MD_FIELD_RANGE(NavAgent, move_speed, F32, 0.1f, 0.f, 10.f),
        MD_FIELD_RANGE(NavAgent, walk_speed, F32, 0.1f, 0.f, 20.f),
        MD_FIELD_RANGE(NavAgent, run_speed,  F32, 0.1f, 0.f, 20.f),
        MD_FIELD(NavAgent, path_len, I32),
        MD_FIELD(NavAgent, path_idx, I32),
        MD_FIELD(NavAgent, crowd_idx, I32),
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

    // Building — skip chain (ProductionChain, complex nested struct).
    static const FieldDesc building_fields[] = {
        MD_FIELD(Building, def_id, U32),
        MD_FIELD(Building, grid_x, I32),
        MD_FIELD(Building, grid_z, I32),
        MD_FIELD(Building, size_x, I32),
        MD_FIELD(Building, size_z, I32),
        MD_FIELD(Building, progress_s, F32),
        MD_FIELD(Building, active, Bool),
        MD_FIELD(Building, phase, Enum8),
        MD_FIELD_RANGE(Building, build_pct, F32, 1.f, 0.f, 100.f),
        MD_FIELD(Building, hp, F32),
        MD_FIELD(Building, hp_max, F32),
        MD_FIELD(Building, power_output, F32),
    };
    r.Register("building", building_fields,
               (int)(sizeof(building_fields) / sizeof(building_fields[0])),
               (uint16_t)sizeof(Building));

    // Inventory — skip item_ids/amounts/weights[64] (parallel slot arrays).
    static const FieldDesc inventory_fields[] = {
        MD_FIELD(Inventory, slot_count, I32),
        MD_FIELD(Inventory, total_weight, F32),
        MD_FIELD(Inventory, max_carry_weight, F32),
    };
    r.Register("inventory", inventory_fields,
               (int)(sizeof(inventory_fields) / sizeof(inventory_fields[0])),
               (uint16_t)sizeof(Inventory));

    // StatSheet — race/traits + 4 core skill summary fields; skip full skills[25].
    static const FieldDesc stat_sheet_fields[] = {
        MD_FIELD(StatSheet, race_id, U8),
        MD_FIELD(StatSheet, traits,  U16),
        MD_FIELD_RANGE(StatSheet, skills[0], U8, 1.f, 0.f, 100.f),  // Strength
        MD_FIELD_RANGE(StatSheet, skills[1], U8, 1.f, 0.f, 100.f),  // Dexterity
        MD_FIELD_RANGE(StatSheet, skills[2], U8, 1.f, 0.f, 100.f),  // Athletics
        MD_FIELD_RANGE(StatSheet, skills[3], U8, 1.f, 0.f, 100.f),  // Toughness
    };
    r.Register("stat_sheet", stat_sheet_fields,
               (int)(sizeof(stat_sheet_fields) / sizeof(stat_sheet_fields[0])),
               (uint16_t)sizeof(StatSheet));

    fprintf(stdout, "[ComponentReflect] %d components registered\n", r.Count());
}

} // namespace md

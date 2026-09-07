#include <monkey_dust/combat/power_system.h>
#include <monkey_dust/combat/power_def.h>
#include <monkey_dust/combat/power_manager.h>
#include <monkey_dust/components/health.h>
#include <monkey_dust/components/projectile.h>
#include <monkey_dust/world/world_transform.h>
#include <monkey_dust/ecs/registry.h>
#include <monkey_dust/ecs/md_registry.h>
#include <monkey_dust/platform/md_log.h>
#include <cmath>
#include <cstring>

namespace md {

bool PowerSystem::Use(MdEntity caster, int power_id, float tx, float tz) {
    const PowerDef* def = PowerManager::Get().Find(power_id);
    if (!def) {
        MD_LOG(MD_LOG_WARNING, "PowerSystem: unknown power id=%d", power_id);
        return false;
    }
    auto& reg = MdRegistry::Get();
    if (!reg.Valid(caster) || !(reg.Handle(caster).has<WorldTransform>())) return false;

    const auto& tr = reg.Handle(caster).get_mut<WorldTransform>();
    float cx = tr.x, cz = tr.z;

    if (strcmp(def->dmg_type, "melee") == 0) {
        float damage = def->radius * 10.0f;
        DoMelee(caster, cx, cz, def->radius, damage);
    } else if (strcmp(def->dmg_type, "ment") == 0) {
        // Magic: AoE centered on target position
        float damage = def->radius * 10.0f;
        DoMelee(caster, tx, tz, def->radius, damage);
    } else if (strcmp(def->dmg_type, "ranged") == 0) {
        SpawnProjectile(caster, cx, cz, tx, tz, power_id);
    }
    return true;
}

void PowerSystem::DoMelee(MdEntity caster, float cx, float cz,
                           float radius, float damage) {
    auto& reg = MdRegistry::Get();
    float r2 = radius * radius;

    // Collect hits first to avoid mutating registry mid-view.
    struct HitRecord { MdEntity e; };
    static HitRecord hits[64];
    int hit_count = 0;

#if defined(MD_ECS_GAIA)
    static auto q_p3_power_system_1 = reg.Raw().query().all<WorldTransform>().all<Health>();
#else
    static auto q_p3_power_system_1 = reg.Raw().query<WorldTransform, Health>();
#endif
    MdEach(q_p3_power_system_1, 
        [&](MdEntity e, const WorldTransform& t, const Health&) {
            if (e == caster || hit_count >= 64) return;
            float dx = t.x - cx, dz = t.z - cz;
            if (dx*dx + dz*dz <= r2) hits[hit_count++] = {e};
        });

    for (int i = 0; i < hit_count; ++i) {
        if (!reg.Valid(hits[i].e) || !(reg.Handle(hits[i].e).has<Health>())) continue;
        auto& hp = reg.Handle(hits[i].e).get_mut<Health>();
        hp.hp[1] -= damage;  // Torso (index 1)
        if (hp.hp[1] < 0.0f) hp.hp[1] = 0.0f;
        hp.UpdateIncap();
    }
}

void PowerSystem::SpawnProjectile(MdEntity caster,
                                   float cx, float cz,
                                   float tx, float tz,
                                   int power_id) {
    const PowerDef* def = PowerManager::Get().Find(power_id);
    auto& reg = MdRegistry::Get();

    constexpr float PROJ_SPEED = 8.0f; // m/s
    float dx = tx - cx, dz = tz - cz;
    float dist = std::sqrt(dx*dx + dz*dz);
    float vx = 0.0f, vz = 0.0f;
    if (dist > 0.01f) { vx = dx/dist * PROJ_SPEED; vz = dz/dist * PROJ_SPEED; }

    ProjectileComponent pc{};
    pc.owner      = caster;
    pc.power_id   = power_id;
    pc.x          = cx;
    pc.z          = cz;
    pc.vx         = vx;
    pc.vz         = vz;
    pc.speed      = PROJ_SPEED;
    pc.damage     = def ? def->radius * 8.0f : 10.0f;
    pc.lifespan_s = def ? def->radius * 0.5f + 1.5f : 2.0f;
    pc.elapsed_s  = 0.0f;
    pc.radius     = 0.4f;

    auto proj = reg.Create();
    reg.Handle(proj).emplace<ProjectileComponent>(pc);
    reg.Handle(proj).emplace<WorldTransform>(WorldTransform{cx, 0.0f, cz, 0.0f});
}

} // namespace md

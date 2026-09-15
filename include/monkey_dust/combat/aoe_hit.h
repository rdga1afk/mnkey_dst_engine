#pragma once
// VBfA-R: AoE damage helpers.
// Include this AFTER damage_calc.h, combat.h, health.h are resolved.
#include <monkey_dust/combat/damage_calc.h>
#include <monkey_dust/ecs/md_registry.h>
#include <monkey_dust/world/world_transform.h>
#include <monkey_dust/components/health.h>
#include <monkey_dust/components/combat.h>
#include <cmath>

// VBfA-R: AoE radius constants (metres)
namespace AoeRadius {
    constexpr float SMALL  = 1.5f;
    constexpr float MEDIUM = 3.0f;
    constexpr float LARGE  = 6.0f;
}

// AoeHit result: collects up to MAX_HITS entities within radius.
struct AoeResult {
    static constexpr int MAX_HITS = 32;
    MdEntity hits[MAX_HITS];
    int          count = 0;
};

// Collect entities with WorldTransform within radius of (ox, oz).
inline AoeResult AoeHit(float ox, float oz, float radius) {
    AoeResult res{};
    auto& reg = MdRegistry::Get();
    float r2  = radius * radius;
    static auto q_p3_aoe_hit_1 = reg.Raw().query().all<WorldTransform>();
    MdEach(q_p3_aoe_hit_1, [&](MdEntity e, const WorldTransform& tr) {
        if (res.count >= AoeResult::MAX_HITS) return;
        float dx = tr.x - ox, dz = tr.z - oz;
        if (dx*dx + dz*dz <= r2)
            res.hits[res.count++] = e;
    });
    return res;
}

// Apply flat damage with linear falloff (1.0 centre → 0.5 edge) to all hits.
// Damage lands on Torso (limb index 1); no zone roll for AoE.
inline void AoeApply(const AoeResult& aoe, float ox, float oz,
                     float radius, float dmg, DamageType type)
{
    auto& reg = MdRegistry::Get();
    WeaponStats wpn{ dmg, type, 0.f, 0u, 0.f };
    for (int i = 0; i < aoe.count; ++i) {
        MdEntity e = aoe.hits[i];
        if (!reg.Valid(e)) continue;
        Health* hp  = reg.Handle(e).try_get_mut<Health>();
        Combat* cmb = reg.Handle(e).try_get_mut<Combat>();
        if (!hp || !cmb) continue;
        const auto& tr = reg.Handle(e).get_mut<WorldTransform>();
        float dx = tr.x - ox, dz = tr.z - oz;
        float dist    = sqrtf(dx*dx + dz*dz);
        float falloff = (radius > 0.f) ? (1.0f - 0.5f * dist / radius) : 1.0f;
        float eff = CalcDamage(wpn, cmb->armor) * falloff;
        hp->hp[1] -= eff;
        if (hp->hp[1] < 0.f) hp->hp[1] = 0.f;
        hp->UpdateIncap();
    }
}

#include <monkey_dust/combat/projectile_system.h>
#include <monkey_dust/components/projectile.h>
#include <monkey_dust/components/health.h>
#include <monkey_dust/world/world_transform.h>
#include <monkey_dust/ecs/md_registry.h>
#include <array>
#include <cmath>

static constexpr int MAX_DESTROY_PER_TICK = 32;

namespace md {

void ProjectileSystem::Tick(float dt) {
    auto& reg = MdRegistry::Get();

    // Collect entities to destroy — avoid mutation during view iteration.
    std::array<MdEntity, MAX_DESTROY_PER_TICK> to_destroy{};
    int destroy_count = 0;

    static auto q_p3_projectile_system_1 = reg.Raw().query().all<ProjectileComponent&>().all<WorldTransform&>();
    MdEach(q_p3_projectile_system_1, 
        [&](MdEntity pe, ProjectileComponent& pc, WorldTransform& pt) {
            if (destroy_count >= MAX_DESTROY_PER_TICK) return;

            pc.elapsed_s += dt;
            if (pc.elapsed_s >= pc.lifespan_s) {
                to_destroy[destroy_count++] = pe;
                return;
            }

            pc.x += pc.vx * dt;
            pc.z += pc.vz * dt;
            pt.x = pc.x;
            pt.z = pc.z;

            // Hit detection: check all Health entities within radius.
            float r2 = pc.radius * pc.radius;
            bool  hit = false;

            // Collect hits to avoid inner mutation.
            static MdEntity hit_ents[8];
            int hit_count = 0;

            static auto q_p3_projectile_system_2 = reg.Raw().query().all<WorldTransform>().all<Health>();
            MdEach(q_p3_projectile_system_2, 
                [&](MdEntity te, const WorldTransform& tr, const Health&) {
                    if (hit || te == pe || te == pc.owner || hit_count >= 8) return;
                    float dx = tr.x - pc.x, dz = tr.z - pc.z;
                    if (dx*dx + dz*dz <= r2) hit_ents[hit_count++] = te;
                });

            if (hit_count > 0) {
                for (int i = 0; i < hit_count; ++i) {
                    if (!reg.Valid(hit_ents[i]) || !(reg.Handle(hit_ents[i]).has<Health>())) continue;
                    auto& hp = reg.Handle(hit_ents[i]).get_mut<Health>();
                    hp.hp[1] -= pc.damage;  // Torso (index 1)
                    if (hp.hp[1] < 0.0f) hp.hp[1] = 0.0f;
                    hp.UpdateIncap();
                }
                hit = true;
            }

            if (hit && destroy_count < MAX_DESTROY_PER_TICK) {
                to_destroy[destroy_count++] = pe;
            }
        });

    for (int i = 0; i < destroy_count; ++i) {
        if (reg.Valid(to_destroy[i])) reg.Destroy(to_destroy[i]);
    }
}

} // namespace md

#pragma once
// RepulsionSystem (VBfA-AI4) — lightweight spring-based NPC separation.
//
// Source: Viking Battle for Asgard REPULSION task (tasks.bin, priority=30).
// VBfA ran REPULSION independently from physics (dependency=START_NEXT_FRAME),
// BEFORE the combat manager, supplying separation forces without Havok/PhysX.
//
// Role in monkey_dust:
//   - ALL NPCs with WorldTransform+NavAgent: RepulsionSystem provides inter-NPC push.
//   - Close NPCs (<30m): Jolt CharacterVirtual also runs (terrain/gravity quality).
//   - Distant NPCs (>80m, Tier3): only RepulsionSystem + CrowdSystem ORCA steering.
//     Jolt SetLinearVelocity = 0 for those (see logic_tick.cpp Jolt loop).
//
// Algorithm: collect→apply (never mutate WorldTransform during view.each iteration).
//   for each NPC: query neighbours in MIN_SEP*1.5m radius
//   if dist < MIN_SEP: accumulate spring push vec (dx/d * push_mag)
//   after loop:       apply all pushes to WorldTransform
//
// Constraints:
//   - Static push buffer (BSS, no heap): max 512 pushes per tick
//   - O(n × k) where k = average neighbours in QUERY_RADIUS (~2-4)
//   - Called once per logic tick, BEFORE JoltWorld::Step

#include <monkey_dust/ecs/registry.h>
#include <monkey_dust/ecs/md_registry.h>
#include <monkey_dust/world/world_transform.h>
#include <monkey_dust/world/spatial_grid.h>
#include <monkey_dust/components/nav_agent.h>
#include <cmath>

class RepulsionSystem {
public:
    // Minimum centre-to-centre distance before push activates (≈ capsule diameter).
    static constexpr float MIN_SEP  = 0.8f;
    // Spring stiffness: push_force = (MIN_SEP - dist) * SPRING_K * dt
    static constexpr float SPRING_K = 2.5f;
    // Spatial query radius: look for neighbours within this distance.
    static constexpr float QUERY_R  = MIN_SEP * 1.5f;  // 1.2m
    // Max pushes accumulated per tick (static BSS buffer — no heap).
    static constexpr int   MAX_PUSH = 512;

    // Call once per logic tick, BEFORE JoltWorld::Step.
    // grid: the scene SpatialGrid (rebuilt each tick by RebuildGrid).
    // dt:   logic tick delta time in seconds (LOGIC_TICK_S = 0.1f).
    void Update(SpatialGrid& grid, float dt) {
        struct Push { MdEntity e; float dx, dz; };
        static Push s_pushes[MAX_PUSH];
        int npushes = 0;

        auto& reg = MdRegistry::Get();

        // Collect: gather push vectors without modifying positions.
        static auto q_p3_repulsion_system_1 = reg.Raw().query().all<WorldTransform>().all<NavAgent>();
        MdEach(q_p3_repulsion_system_1, [&](MdEntity a, const WorldTransform& ta, const NavAgent&) {
               MdEntity near[8];
               int nc = grid.QueryRadius(ta.x, ta.z, QUERY_R, near, 8);
               for (int i = 0; i < nc && npushes < MAX_PUSH - 1; ++i) {
                   if (!reg.Valid(near[i]) || near[i] == a) continue;
                   const auto* tb = reg.Handle(near[i]).try_get_mut<WorldTransform>();
                   if (!tb) continue;
                   float dx = ta.x - tb->x;
                   float dz = ta.z - tb->z;
                   float d2 = dx * dx + dz * dz;
                   if (d2 >= MIN_SEP * MIN_SEP || d2 < 1e-6f) continue;
                   float d    = sqrtf(d2);
                   float mag  = (MIN_SEP - d) * SPRING_K * dt;
                   float inv  = mag / d;
                   s_pushes[npushes++] = { a, dx * inv, dz * inv };
               }
           });

        // Apply: all position mutations happen after the view.each completes.
        for (int i = 0; i < npushes; ++i) {
            if (!reg.Valid(s_pushes[i].e)) continue;
            auto* t = reg.Handle(s_pushes[i].e).try_get_mut<WorldTransform>();
            if (t) { t->x += s_pushes[i].dx; t->z += s_pushes[i].dz; }
        }
    }
};

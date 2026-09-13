#pragma once
#include <monkey_dust/ecs/md_entity.h>

struct ProjectileComponent {
    MdEntity owner;      // caster entity (excluded from hit checks)
    int          power_id = 0;   // source power (for FX lookup)
    float        x = 0.f, z = 0.f;       // current world position (mirrored to WorldTransform)
    float        vx = 0.f, vz = 0.f;     // velocity m/s
    float        speed = 0.f;      // scalar speed m/s
    float        damage = 0.f;
    float        lifespan_s = 0.f;
    float        elapsed_s = 0.f;
    float        radius = 0.f;     // hit sphere radius (m)
};

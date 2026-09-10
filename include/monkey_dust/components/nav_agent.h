#pragma once
#include <monkey_dust/nav/path_cache.h>

// alignas(16) REMOVED (2026-09-08): was intended for SSE loads on
// target_x/z and desired_vel_x/z (VBfA RE §8), but no such SIMD access
// exists anywhere in the codebase today (grepped engine/ + game/ for
// _mm_load_ps/_mm_store_ps -- only particle_soa.cpp/transform_soa.cpp use
// them, both on separate SoA arrays, not this struct) -- the alignment
// was aspirational and unused. Confirmed via a minimal standalone
// gaia-ecs repro (see docs/GAIA_ALIGNAS_BUG.md) that
// gaia::ecs::World::add<T>(entity, value) segfaults (null m_pChunk deref
// inside ComponentSetter::sset, gaia.h:46061) for any alignas(16)+
// component type added as a NON-FIRST component on an entity -- every
// real NPC's NavAgent is always added after other components, so this
// was a hard blocker under MD_ECS_GAIA. If SSE loads on these fields are
// ever actually implemented, use _mm_loadu_ps (unaligned, same result)
// instead of re-adding alignas.
struct NavAgent {
    float    target_x, target_z;
    float    path[MAX_PATH_LEN * 3];
    int      path_len;
    int      path_idx;
    bool     is_moving     = false;  // set by actMoveToTarget, read by animator
    float    move_speed    = 0.0f;   // 0=still, 1=walk, 2=run (for blend)
    float    walk_speed    = 1.7f;   // m/s — Kenshi walk ~5.5 km/h (was 3.5)
    float    run_speed     = 4.5f;   // m/s — Kenshi run ~16 km/h (was 7.0)
    int      crowd_idx     = -1;     // dtCrowd agent index; -1 = not in crowd
    float    desired_vel_x = 0.0f;   // set by CrowdSystem, read by JoltWorld + render lerp
    float    desired_vel_z = 0.0f;
    float    render_x      = 0.0f;   // render-rate extrapolated position (smoothing)
    float    render_z      = 0.0f;
};

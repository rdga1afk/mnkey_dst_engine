#pragma once
// JoltWorld — singleton Jolt Physics world: raw PhysicsSystem + ragdoll.
// NPC physics goes through System() (RagdollSystem) directly, not through
// a CharacterVirtual controller -- PhysicsAgent::character below is read
// by game/src's logic ticks but nothing currently assigns it (the
// CreateCharacter subsystem that used to populate it was confirmed
// unreachable and removed 2026-09-25, surgical-simplicity audit).

// Jolt requires these macros before the header
#ifndef JPH_DEBUG_RENDERER
#define JPH_DEBUG_RENDERER 0
#endif

#include <Jolt/Jolt.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/Physics/Character/CharacterVirtual.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Core/JobSystemThreadPool.h>

class JoltWorld {
public:
    static JoltWorld& Get() { static JoltWorld s; return s; }

    // Call once after window+GPU ready. max_bodies limits pre-alloc.
    void Init(int max_bodies = 1024);
    void Shutdown();
    bool IsReady() const { return ready_; }

    // Step the physics world (call at logic tick rate, 0.1s).
    void Step(float dt);

    // Add terrain chunk as a static MeshShape from its heightmap.
    // heights: (grid+1)×(grid+1) row-major floats.
    // Returns the BodyID (use RemoveBody to remove during streaming).
    JPH::BodyID AddTerrainMesh(const float* heights, int grid,
                               float cell_size,
                               float origin_x, float origin_z);
    void RemoveBody(JPH::BodyID id);

    // Add a static AABB box (G-1 prop collision). Centre cx/cy/cz, half-extents hx/hy/hz.
    JPH::BodyID AddStaticBox(float cx, float cy, float cz,
                              float hx, float hy, float hz);

    // P-NG-6.3: Replace the single PCG terrain body with a HeightFieldShape.
    // hmap: samples×samples row-major floats (metres). scale_xz: metres per cell.
    // Internally downsamples to 129×129 (HeightFieldShape constraint).
    // No-op if not initialized. Removes previous terrain body first.
    void ReplaceTerrainBody(const float* hmap, int samples,
                            float scale_xz, float off_x, float off_z);

    // Cast a ray from (fx,fy,fz) toward (tx,ty,tz).
    // Returns fraction [0,1] of first hit, or 1.0 if no hit.
    float CastRay(float fx, float fy, float fz,
                  float tx, float ty, float tz);

    JPH::PhysicsSystem& System() { return physics_system_; }

private:
    JoltWorld() = default;

    bool                          ready_ = false;
    JPH::PhysicsSystem            physics_system_;
    JPH::TempAllocatorImpl*       temp_alloc_  = nullptr;
    JPH::JobSystemThreadPool*     job_system_  = nullptr;

    // Broad-phase layers
    struct BPLayerInterface;
    struct OVBPLayerPair;
    struct OVBroadPhase;
    BPLayerInterface* bp_layer_iface_  = nullptr;
    OVBPLayerPair*    ovbp_layer_pair_ = nullptr;
    OVBroadPhase*     ovbp_filter_     = nullptr;

    JPH::BodyID terrain_body_;  // current PCG terrain body (invalid = none)
};

// Per-NPC ECS component
struct PhysicsAgent {
    JPH::CharacterVirtual* character      = nullptr;
    float                  desired_vel_x  = 0.f;
    float                  desired_vel_z  = 0.f;
    bool                   on_ground      = true;
};

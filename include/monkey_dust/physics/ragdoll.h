#pragma once
// RagdollSystem — Jolt-based ragdoll for death/KO NPCs.
// Phase 3, Task 3.2.
//
// LOD policy: ragdoll active only within 30m of player; beyond that a
// death-pose static mesh is sufficient. On chunk unload all ragdolls
// in that chunk are deactivated.
//
// Build path:
//   Jolt::RagdollSettings is loaded once at startup from prebuilt binary
//   (or built procedurally from md_human.glb skeleton via RagdollSystem::BuildSettings).
//   Each dead NPC gets a Ragdoll instance via RagdollSettings::CreateRagdoll().

#ifndef JPH_DEBUG_RENDERER
#define JPH_DEBUG_RENDERER 0
#endif

#include <Jolt/Jolt.h>
#include <Jolt/Physics/Ragdoll/Ragdoll.h>
#include <Jolt/Skeleton/Skeleton.h>
#include <Jolt/Skeleton/SkeletonPose.h>
#include <monkey_dust/ecs/md_registry.h>
#include <monkey_dust/physics/jolt_world.h>

// Max simultaneous ragdolls (LOD ≤30m cap).
static constexpr int MAX_RAGDOLLS = 16;

// ── Per-entity ECS component ──────────────────────────────────────────────────
struct RagdollComponent {
    JPH::Ragdoll* ragdoll = nullptr;  // null when inactive / beyond LOD
    bool          active  = false;
    float         time_active_s = 0.f;  // for sleep/removal after settling
};

// ── RagdollSystem singleton ───────────────────────────────────────────────────
// Call Init() once after JoltWorld::Init().
// Call Tick(dt, player_x, player_z) from logic tick.
class RagdollSystem {
public:
    static RagdollSystem& Get() { static RagdollSystem s; return s; }

    // Load or build RagdollSettings from skeleton data.
    // glb_path: path to md_human.glb (used to derive bone hierarchy).
    // Returns false if Jolt not ready.
    bool Init(const char* glb_path);

    // Per logic-tick (0.1s). Activates ragdolls for newly-dead nearby NPCs;
    // deactivates those beyond LOD_RADIUS_M; removes settled ragdolls.
    void Tick(float dt, float player_x, float player_z);

    // Force-deactivate all ragdolls in a chunk (call on chunk unload).
    // chunk_min_x/z, chunk_max_x/z define the chunk bounds.
    void DeactivateChunk(float min_x, float min_z, float max_x, float max_z);

    // World transforms (column-major mat4) for each of the 6 ragdoll segments.
    // out_seg[0]=Torso [1]=Head [2]=LArm [3]=RArm [4]=LLeg [5]=RLeg
    // Returns false if entity has no active ragdoll.
    bool GetSegmentWorlds(MdEntity e, float out_seg[6][16]) const;

    // Retrieve bone world matrices for GPU skinning upload (30 bones × Mat4).
    // out_matrices: caller-allocated float[bone_count * 16].
    // Returns false if entity has no active ragdoll.
    bool GetBoneMatrices(MdEntity e, float* out_matrices, int bone_count) const;

    bool IsReady() const { return settings_ != nullptr; }

    static constexpr float LOD_RADIUS_M     = 30.f;
    static constexpr float SLEEP_SETTLE_S   = 5.f;   // deactivate after ragdoll settles

private:
    RagdollSystem() = default;

    void Activate  (MdEntity e, MdRegistry& reg);
    void Deactivate(MdEntity e, MdRegistry& reg);

    JPH::Ref<JPH::RagdollSettings> settings_;
    JPH::Ref<JPH::Skeleton>        skeleton_;

    // GroupID counter — must be unique per ragdoll instance in PhysicsSystem.
    JPH::CollisionGroup::GroupID   next_group_id_ = 1;
};

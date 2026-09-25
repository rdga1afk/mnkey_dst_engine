// JoltWorld — Jolt Physics character controller + ragdoll foundation.
// References: https://jrouwe.github.io/JoltPhysics/

// Must be first
#include <Jolt/Jolt.h>
#include <Jolt/RegisterTypes.h>
#include <Jolt/Core/Factory.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Core/JobSystemThreadPool.h>
#include <Jolt/Physics/PhysicsSettings.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyActivationListener.h>
#include <Jolt/Physics/Collision/Shape/MeshShape.h>
#include <Jolt/Physics/Collision/Shape/HeightFieldShape.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/RotatedTranslatedShape.h>
#include <Jolt/Physics/Character/CharacterVirtual.h>
#include <Jolt/Physics/Character/CharacterBase.h>
#include <Jolt/Physics/EPhysicsUpdateError.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/CastResult.h>

#include <monkey_dust/physics/jolt_world.h>
#include <monkey_dust/md_config.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>

// ── Jolt boilerplate: object layers ─────────────────────────────────────────
// RAGDOLL layer: collides only with other RAGDOLL — skips all 49 terrain MeshShapes.
// Ragdolls fall under gravity + ConeConstraints without expensive terrain narrowphase.
namespace Layers {
    static constexpr JPH::ObjectLayer NON_MOVING = 0;
    static constexpr JPH::ObjectLayer MOVING     = 1;
    static constexpr JPH::ObjectLayer RAGDOLL    = 2;
    static constexpr JPH::ObjectLayer NUM_LAYERS = 3;
}
namespace BroadPhaseLayers {
    static constexpr JPH::BroadPhaseLayer NON_MOVING(0);
    static constexpr JPH::BroadPhaseLayer MOVING(1);
    static constexpr uint NUM_LAYERS = 2;
}

struct JoltWorld::BPLayerInterface final : public JPH::BroadPhaseLayerInterface {
    JPH::BroadPhaseLayer obj_to_bp[Layers::NUM_LAYERS];
    BPLayerInterface() {
        obj_to_bp[Layers::NON_MOVING] = BroadPhaseLayers::NON_MOVING;
        obj_to_bp[Layers::MOVING]     = BroadPhaseLayers::MOVING;
        obj_to_bp[Layers::RAGDOLL]    = BroadPhaseLayers::MOVING;
    }
    uint GetNumBroadPhaseLayers() const override { return BroadPhaseLayers::NUM_LAYERS; }
    JPH::BroadPhaseLayer GetBroadPhaseLayer(JPH::ObjectLayer l) const override {
        return obj_to_bp[l];
    }
#if defined(JPH_EXTERNAL_PROFILE) || defined(JPH_PROFILE_ENABLED)
    const char* GetBroadPhaseLayerName(JPH::BroadPhaseLayer l) const override {
        return l == BroadPhaseLayers::NON_MOVING ? "NON_MOVING" : "MOVING";
    }
#endif
};

struct JoltWorld::OVBPLayerPair final : public JPH::ObjectVsBroadPhaseLayerFilter {
    bool ShouldCollide(JPH::ObjectLayer ol, JPH::BroadPhaseLayer bl) const override {
        if (ol == Layers::NON_MOVING) return bl == BroadPhaseLayers::MOVING;
        if (ol == Layers::RAGDOLL)    return bl == BroadPhaseLayers::MOVING;
        return true;
    }
};

struct JoltWorld::OVBroadPhase final : public JPH::ObjectLayerPairFilter {
    bool ShouldCollide(JPH::ObjectLayer a, JPH::ObjectLayer b) const override {
        if (a == Layers::NON_MOVING) return b == Layers::MOVING;
        if (a == Layers::MOVING)     return b != Layers::RAGDOLL;
        if (a == Layers::RAGDOLL)    return b == Layers::RAGDOLL;
        return false;
    }
};

// ── Init / Shutdown ──────────────────────────────────────────────────────────
void JoltWorld::Init(int max_bodies) {
    // max_bodies=0 → use value from md_config.txt
    if (max_bodies <= 0) max_bodies = MdConfig::Get().max_bodies;
    JPH::RegisterDefaultAllocator();
    JPH::Factory::sInstance = new JPH::Factory();
    JPH::RegisterTypes();

    // Kenshi pattern: physics thread counts from config (internalThreadCount, threadMask).

    const MdConfig& cfg = MdConfig::Get();
    const int temp_mb   = cfg.physics_temp_mb;   // default 8
    const int workers   = cfg.physics_threads;   // default 2 (deadlock-safe minimum)
    temp_alloc_  = new JPH::TempAllocatorImpl((uint32_t)(temp_mb * 1024 * 1024));
    // workers ≥ 2: avoids 1-thread pool deadlock (main waits for worker,
    // worker creates job that needs a second worker to signal the barrier).
    job_system_  = new JPH::JobSystemThreadPool(JPH::cMaxPhysicsJobs,
                                                JPH::cMaxPhysicsBarriers,
                                                workers);
    fprintf(stdout, "[Jolt] workers=%d  temp_alloc=%dMB\n", workers, temp_mb);
    bp_layer_iface_ = new BPLayerInterface();
    ovbp_layer_pair_= new OVBPLayerPair();
    ovbp_filter_    = new OVBroadPhase();

    const uint max_body_pairs   = (uint)max_bodies * 2;
    const uint max_constraints  = (uint)max_bodies;
    physics_system_.Init((uint)max_bodies, 0, max_body_pairs, max_constraints,
                         *bp_layer_iface_, *ovbp_layer_pair_, *ovbp_filter_);

    ready_ = true;
    fprintf(stdout, "[Jolt] Init OK  max_bodies=%d\n", max_bodies);
}

void JoltWorld::Shutdown() {
    delete bp_layer_iface_;   bp_layer_iface_  = nullptr;
    delete ovbp_layer_pair_;  ovbp_layer_pair_ = nullptr;
    delete ovbp_filter_;      ovbp_filter_     = nullptr;
    delete temp_alloc_;       temp_alloc_      = nullptr;
    delete job_system_;       job_system_      = nullptr;
    JPH::UnregisterTypes();
    delete JPH::Factory::sInstance; JPH::Factory::sInstance = nullptr;
    ready_ = false;
}

// ── Step ────────────────────────────────────────────────────────────────────
void JoltWorld::Step(float dt) {
    if (!ready_) return;
    physics_system_.Update(dt, 1, temp_alloc_, job_system_);
}

// ── Terrain chunk as static MeshShape ────────────────────────────────────────
JPH::BodyID JoltWorld::AddTerrainMesh(const float* heights, int grid,
                                       float cell_size,
                                       float origin_x, float origin_z)
{
    if (!ready_) return JPH::BodyID();
    // HeightFieldShape instead of MeshShapeSettings/triangle-soup — confirmed
    // by direct measurement to be the dominant startup cost (3.86s across 81
    // chunks, ~48ms/chunk) building an unoptimized BVH over grid*grid*2
    // (128*128*2=32768) arbitrary triangles per chunk, when `heights` is
    // already a regular (grid+1)x(grid+1) height grid — exactly what
    // HeightFieldShape is purpose-built for (same pattern already proven in
    // ReplaceTerrainBody below). Requires (grid % block_size(=2)) == 0;
    // TERRAIN_GRID=128 satisfies this with no downsampling needed, unlike
    // ReplaceTerrainBody's arbitrary-resolution input.
    JPH::HeightFieldShapeSettings hfs(
        heights,
        JPH::Vec3(origin_x, 0.f, origin_z),
        JPH::Vec3(cell_size, 1.f, cell_size),
        (JPH::uint32)(grid + 1));
    auto result = hfs.Create();
    if (result.HasError()) {
        fprintf(stderr, "[Jolt] TerrainMesh(HeightField) failed: %s\n", result.GetError().c_str());
        return JPH::BodyID();
    }
    JPH::BodyCreationSettings bcs(result.Get(),
        JPH::RVec3::sZero(), JPH::Quat::sIdentity(),
        JPH::EMotionType::Static, Layers::NON_MOVING);
    return physics_system_.GetBodyInterface().CreateAndAddBody(
        bcs, JPH::EActivation::DontActivate);
}

void JoltWorld::RemoveBody(JPH::BodyID id)
{
    if (!ready_ || id.IsInvalid()) return;
    auto& bi = physics_system_.GetBodyInterface();
    bi.RemoveBody(id);
    bi.DestroyBody(id);
}

JPH::BodyID JoltWorld::AddStaticBox(float cx, float cy, float cz,
                                     float hx, float hy, float hz)
{
    if (!ready_) return JPH::BodyID();
    JPH::BoxShapeSettings bss(JPH::Vec3(hx, hy, hz));
    auto result = bss.Create();
    if (result.HasError()) return JPH::BodyID();
    JPH::BodyCreationSettings bcs(result.Get(),
        JPH::RVec3((JPH::Real)cx, (JPH::Real)cy, (JPH::Real)cz),
        JPH::Quat::sIdentity(),
        JPH::EMotionType::Static, Layers::NON_MOVING);
    return physics_system_.GetBodyInterface().CreateAndAddBody(
        bcs, JPH::EActivation::DontActivate);
}

// P-NG-6.3: Replace PCG terrain body with HeightFieldShape (more efficient than trimesh).
// Downsamples hmap from samples×samples → 129×129 (HeightFieldShape requires (N-1)%2==0).
void JoltWorld::ReplaceTerrainBody(const float* hmap, int samples,
                                    float scale_xz, float off_x, float off_z)
{
    if (!ready_) return;

    // Remove previous terrain body
    if (!terrain_body_.IsInvalid()) {
        auto& bi = physics_system_.GetBodyInterface();
        bi.RemoveBody(terrain_body_);
        bi.DestroyBody(terrain_body_);
        terrain_body_ = JPH::BodyID();
    }

    // HeightFieldShape requires (N-1) divisible by block_size (default 2) → use N=129.
    static constexpr int HF_RES = 129;
    static float s_hf[HF_RES * HF_RES];
    float inv = (float)(samples - 1) / (float)(HF_RES - 1);
    for (int z = 0; z < HF_RES; ++z)
        for (int x = 0; x < HF_RES; ++x) {
            int sx = (int)(x * inv + 0.5f); if (sx >= samples) sx = samples - 1;
            int sz = (int)(z * inv + 0.5f); if (sz >= samples) sz = samples - 1;
            s_hf[z * HF_RES + x] = hmap[sz * samples + sx];
        }

    float hf_cell = scale_xz * (float)(samples - 1) / (float)(HF_RES - 1);
    JPH::HeightFieldShapeSettings hfs(
        s_hf,
        JPH::Vec3(off_x, 0.f, off_z),
        JPH::Vec3(hf_cell, 1.f, hf_cell),
        (JPH::uint32)HF_RES);

    auto result = hfs.Create();
    if (result.HasError()) {
        fprintf(stderr, "[Jolt] HeightField: %s\n", result.GetError().c_str());
        return;
    }

    JPH::BodyCreationSettings bcs(result.Get(),
        JPH::RVec3::sZero(), JPH::Quat::sIdentity(),
        JPH::EMotionType::Static, Layers::NON_MOVING);
    terrain_body_ = physics_system_.GetBodyInterface().CreateAndAddBody(
        bcs, JPH::EActivation::DontActivate);
}

float JoltWorld::CastRay(float fx, float fy, float fz,
                          float tx, float ty, float tz)
{
    if (!ready_) return 1.f;
    JPH::Vec3 origin(fx, fy, fz);
    JPH::Vec3 dir(tx - fx, ty - fy, tz - fz);
    JPH::RRayCast ray(origin, dir);
    JPH::RayCastResult hit;
    if (physics_system_.GetNarrowPhaseQuery().CastRay(
            ray, hit,
            physics_system_.GetDefaultBroadPhaseLayerFilter(Layers::NON_MOVING),
            physics_system_.GetDefaultLayerFilter(Layers::NON_MOVING)))
        return hit.mFraction;
    return 1.f;
}


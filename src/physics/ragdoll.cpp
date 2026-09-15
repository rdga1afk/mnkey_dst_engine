#include <monkey_dust/physics/ragdoll.h>
#include <monkey_dust/components/health.h>
#include <monkey_dust/components/agent_state.h>
#include <monkey_dust/world/world_transform.h>
#include <monkey_dust/ecs/md_registry.h>
#include <monkey_dust/platform/md_log.h>

#ifndef JPH_DEBUG_RENDERER
#define JPH_DEBUG_RENDERER 0
#endif
#include <Jolt/Jolt.h>
#include <Jolt/Physics/Ragdoll/Ragdoll.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Constraints/SwingTwistConstraint.h>
#include <Jolt/Physics/Constraints/PointConstraint.h>
#include <Jolt/Physics/Constraints/ConeConstraint.h>
#include <Jolt/Skeleton/Skeleton.h>
#include <Jolt/Skeleton/SkeletonPose.h>
#include <Jolt/Physics/EActivation.h>
#include <cstring>
#include <cmath>

// ── Simplified 6-segment skeleton: root=Torso, children=Head,LArm,RArm,LLeg,RLeg ──
//
// Proportions are approximate for 1.8m character at world scale.
// Full 30-bone ragdoll deferred to Phase 4 content pipeline.
//
//   Segment  | idx | shape           | parent
//   Torso    |  0  | Capsule r=0.15 h=0.5 | root (no constraint)
//   Head     |  1  | Capsule r=0.10 h=0.2 | Torso (swing-twist)
//   LArm     |  2  | Capsule r=0.06 h=0.5 | Torso (swing-twist)
//   RArm     |  3  | Capsule r=0.06 h=0.5 | Torso (swing-twist)
//   LLeg     |  4  | Capsule r=0.08 h=0.6 | Torso (swing-twist)
//   RLeg     |  5  | Capsule r=0.08 h=0.6 | Torso (swing-twist)

static const char* k_joint_names[6] = {
    "Torso","Head","LArm","RArm","LLeg","RLeg"
};
static const int k_parent[6] = { -1, 0, 0, 0, 0, 0 };

// Capsule half-height (m) and radius (m)
static const float k_half_h[6] = { 0.25f, 0.10f, 0.25f, 0.25f, 0.30f, 0.30f };
static const float k_radius[6] = { 0.15f, 0.10f, 0.06f, 0.06f, 0.08f, 0.08f };

// Joint offset from parent origin (Y axis up)
static const float k_offset_y[6] = { 0.f, 0.55f, 0.f, 0.f, -0.55f, -0.55f };
static const float k_offset_x[6] = { 0.f, 0.f, -0.25f, 0.25f, -0.15f, 0.15f };

bool RagdollSystem::Init(const char* /*glb_path*/) {
    if (!JoltWorld::Get().IsReady()) {
        MD_LOG(MD_LOG_WARNING, "RagdollSystem: JoltWorld not ready");
        return false;
    }

    // Build skeleton
    skeleton_ = new JPH::Skeleton;
    for (int i = 0; i < 6; ++i)
        skeleton_->AddJoint(k_joint_names[i], k_parent[i]);

    // Build RagdollSettings — one Part per joint
    settings_ = new JPH::RagdollSettings;
    settings_->mSkeleton = skeleton_;
    settings_->mParts.resize(6);

    JPH::PhysicsSystem& ps = JoltWorld::Get().System();

    for (int i = 0; i < 6; ++i) {
        auto& part = settings_->mParts[i];

        // Capsule shape for this segment
        JPH::CapsuleShapeSettings cs(k_half_h[i], k_radius[i]);
        auto shape_res = cs.Create();
        if (shape_res.HasError()) {
            MD_LOG(MD_LOG_WARNING, "RagdollSystem: capsule %d error: %s",
                   i, shape_res.GetError().c_str());
            return false;
        }
        part.SetShape(shape_res.Get());

        // Initial position (origin; will be overridden by Activate)
        part.mPosition = JPH::RVec3(k_offset_x[i], k_offset_y[i], 0.f);
        part.mMotionType = (i == 0)
            ? JPH::EMotionType::Dynamic
            : JPH::EMotionType::Dynamic;
        part.mObjectLayer = 2;  // RAGDOLL layer — no terrain collision, only RAGDOLL vs RAGDOLL

        // ConeConstraint: angular limit without twist axis — simpler than SwingTwist.
        if (k_parent[i] >= 0) {
            auto* cc = new JPH::ConeConstraintSettings;
            cc->mPoint1       = JPH::RVec3(k_offset_x[i], k_offset_y[i], 0.f);
            cc->mPoint2       = JPH::RVec3(0.f, 0.f, 0.f);
            cc->mTwistAxis1   = JPH::Vec3(0.f, 1.f, 0.f);
            cc->mTwistAxis2   = JPH::Vec3(0.f, 1.f, 0.f);
            cc->mHalfConeAngle = 0.5f; // ~30°
            part.mToParent = cc;
        }
    }

    settings_->DisableParentChildCollisions();

    MD_LOG(MD_LOG_INFO, "RagdollSystem: initialised (6-segment skeleton)");
    return true;
}

void RagdollSystem::Activate(MdEntity e, MdRegistry& reg) {
    if (!settings_) return;

    const WorldTransform* tr = reg.Handle(e).try_get_mut<WorldTransform>();
    if (!tr) return;

    if (!reg.Handle(e).has<RagdollComponent>()) reg.Handle(e).emplace<RagdollComponent>();
    auto& rc = reg.Handle(e).get_mut<RagdollComponent>();
    if (rc.active) return;

    JPH::Ragdoll* rd = settings_->CreateRagdoll(
        next_group_id_++, (JPH::uint64)e.ToIntegral(), &JoltWorld::Get().System());
    if (!rd) return;

    // Position root (Torso) at entity world position
    JPH::SkeletonPose pose;
    pose.SetSkeleton(skeleton_);
    auto& root_jt = pose.GetJoint(0);
    root_jt.mTranslation = JPH::Vec3(tr->x, tr->y + 0.8f, tr->z);
    root_jt.mRotation    = JPH::Quat::sIdentity();
    for (int i = 1; i < 6; ++i) {
        auto& jt = pose.GetJoint(i);
        jt.mTranslation = JPH::Vec3(k_offset_x[i], k_offset_y[i], 0.f);
        jt.mRotation    = JPH::Quat::sIdentity();
    }
    rd->SetPose(pose);
    rd->AddToPhysicsSystem(JPH::EActivation::Activate);

    rc.ragdoll = rd;
    rc.active  = true;
    rc.time_active_s = 0.f;
}

void RagdollSystem::Deactivate(MdEntity e, MdRegistry& reg) {
    auto* rc = reg.Handle(e).try_get_mut<RagdollComponent>();
    if (!rc || !rc->active || !rc->ragdoll) return;
    rc->ragdoll->RemoveFromPhysicsSystem();
    delete rc->ragdoll;
    rc->ragdoll = nullptr;
    rc->active  = false;
}

void RagdollSystem::Tick(float dt, float player_x, float player_z) {
    if (!settings_) return;

    auto& reg = MdRegistry::Get();
    const float lod2 = LOD_RADIUS_M * LOD_RADIUS_M;

    // Activate dead NPCs within LOD range; deactivate those beyond.
    // Collect-then-apply: Activate() does reg.Handle(e).emplace<RagdollComponent>()
    // on first activation, a structural change to e's archetype -- doing that
    // directly inside this query's .each() mutates the same registry being
    // iterated (house rule violation; combat_dispatch.h documents the same
    // collect-then-apply pattern for exactly this reason).
    static auto q_lod = reg.Raw().query().all<LimbHealth&>().all<WorldTransform&>();
    MdEntity to_activate[MAX_RAGDOLLS], to_deactivate[MAX_RAGDOLLS];
    int n_activate = 0, n_deactivate = 0;
    MdEach(q_lod, [&](MdEntity e, LimbHealth& lh, WorldTransform& tr) {
        if (!lh.incapacitated) return;

        float dx = tr.x - player_x;
        float dz = tr.z - player_z;
        float d2 = dx * dx + dz * dz;

        auto* rc = reg.Handle(e).try_get_mut<RagdollComponent>();
        bool  in_range = (d2 <= lod2);

        if (in_range && (!rc || !rc->active)) {
            if (n_activate < MAX_RAGDOLLS) to_activate[n_activate++] = e;
        } else if (!in_range && rc && rc->active) {
            if (n_deactivate < MAX_RAGDOLLS) to_deactivate[n_deactivate++] = e;
        }
    });
    for (int i = 0; i < n_activate; ++i)   Activate(to_activate[i], reg);
    for (int i = 0; i < n_deactivate; ++i) Deactivate(to_deactivate[i], reg);

    // Advance timers on active ragdolls; remove after SLEEP_SETTLE_S.
    static auto q_active_ragdolls = reg.Raw().query().all<RagdollComponent&>();
    MdEach(q_active_ragdolls, [&](MdEntity e, RagdollComponent& rc) {
        if (!rc.active) return;
        rc.time_active_s += dt;
        if (rc.time_active_s > SLEEP_SETTLE_S) {
            Deactivate(e, reg);
        }
    });
}

// ── Segment / bone readback ──────────────────────────────────────────────────

static void jolt_to_mat4(JPH::RVec3 pos, JPH::Quat rot, float* m) {
    float qx=rot.GetX(), qy=rot.GetY(), qz=rot.GetZ(), qw=rot.GetW();
    float x2=qx+qx, y2=qy+qy, z2=qz+qz;
    float xx=qx*x2, xy=qx*y2, xz=qx*z2;
    float yy=qy*y2, yz=qy*z2, zz=qz*z2;
    float wx=qw*x2, wy=qw*y2, wz=qw*z2;
    m[ 0]=1.f-(yy+zz); m[ 1]=xy+wz;        m[ 2]=xz-wy;        m[ 3]=0.f;
    m[ 4]=xy-wz;       m[ 5]=1.f-(xx+zz);  m[ 6]=yz+wx;        m[ 7]=0.f;
    m[ 8]=xz+wy;       m[ 9]=yz-wx;        m[10]=1.f-(xx+yy);  m[11]=0.f;
    m[12]=(float)pos.GetX(); m[13]=(float)pos.GetY(); m[14]=(float)pos.GetZ(); m[15]=1.f;
}

bool RagdollSystem::GetSegmentWorlds(MdEntity e, float out_seg[6][16]) const {
    auto& reg = MdRegistry::Get();
    const auto* rc = reg.Handle(e).try_get_mut<RagdollComponent>();
    if (!rc || !rc->active || !rc->ragdoll) return false;

    const auto& ids = rc->ragdoll->GetBodyIDs();
    auto& bi = JoltWorld::Get().System().GetBodyInterface();
    for (int s = 0; s < 6; ++s) {
        if (s >= (int)ids.size() || ids[s].IsInvalid()) {
            float* m = out_seg[s];
            memset(m, 0, 64);
            m[0]=m[5]=m[10]=m[15]=1.f;
            continue;
        }
        jolt_to_mat4(bi.GetPosition(ids[s]), bi.GetRotation(ids[s]), out_seg[s]);
    }
    return true;
}

// bone_count bones, each mapped to one of 6 segments.
// Torso=0: bones 0,1,12,13,14  Head=1: 20-24  LArm=2: 15-19
// RArm=3: 25-29  LLeg=4: 2-6  RLeg=5: 7-11
static const uint8_t k_bone_seg[30] = {
    0,0, 4,4,4,4,4, 5,5,5,5,5, 0,0,0, 2,2,2,2,2, 1,1,1,1,1, 3,3,3,3,3
};

bool RagdollSystem::GetBoneMatrices(MdEntity e, float* out, int bone_count) const {
    float seg[6][16];
    if (!GetSegmentWorlds(e, seg)) return false;
    const int n = bone_count < 30 ? bone_count : 30;
    for (int b = 0; b < n; ++b)
        memcpy(out + b * 16, seg[k_bone_seg[b]], 64);
    return true;
}

void RagdollSystem::DeactivateChunk(float min_x, float min_z,
                                    float max_x, float max_z) {
    auto& reg = MdRegistry::Get();
    static auto q_chunk = reg.Raw().query().all<RagdollComponent&>().all<WorldTransform&>();
    MdEach(q_chunk, [&](MdEntity e, RagdollComponent& rc, WorldTransform& tr) {
        if (!rc.active) return;
        if (tr.x >= min_x && tr.x <= max_x && tr.z >= min_z && tr.z <= max_z)
            Deactivate(e, reg);
    });
}


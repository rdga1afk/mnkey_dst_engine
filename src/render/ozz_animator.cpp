#include <monkey_dust/render/ozz_animator.h>
#include <monkey_dust/render/skin_mesh.h>    // full SkinMesh definition

static_assert(OZZ_ANIM_MAX_BONES == MAX_SKIN_BONES,
    "OZZ_ANIM_MAX_BONES must equal MAX_SKIN_BONES in skin_mesh.h");

#include <ozz/animation/offline/raw_skeleton.h>
#include <ozz/animation/offline/raw_animation.h>
#include <ozz/animation/offline/skeleton_builder.h>
#include <ozz/animation/offline/animation_builder.h>
#include <ozz/base/maths/simd_math.h>

#include <cstring>
#include <cmath>
#include <unordered_map>

// ── Math helpers ──────────────────────────────────────────────────────────────

static void mat4_identity(float* m) {
    memset(m, 0, 64);
    m[0] = m[5] = m[10] = m[15] = 1.f;
}

// ── RawSkeleton construction ──────────────────────────────────────────────────

static void BuildJoint(const SkinMesh& mesh, int bone_idx,
                       ozz::animation::offline::RawSkeleton::Joint& joint) {
    joint.name = mesh.bone_names[bone_idx];

    const float* bt = mesh.GpuBindT(bone_idx);
    const float* bq = mesh.GpuBindQ(bone_idx);
    const float* bs = mesh.GpuBindS(bone_idx);
    joint.transform.translation = bt ? ozz::math::Float3{bt[0], bt[1], bt[2]}
                                     : ozz::math::Float3{0,0,0};
    joint.transform.rotation    = bq ? ozz::math::Quaternion{bq[0], bq[1], bq[2], bq[3]}
                                     : ozz::math::Quaternion::identity();
    joint.transform.scale       = bs ? ozz::math::Float3{bs[0], bs[1], bs[2]}
                                     : ozz::math::Float3{1,1,1};

    for (int b = 0; b < mesh.bone_count; ++b) {
        if (mesh.GpuParent(b) == bone_idx) {
            joint.children.emplace_back();
            BuildJoint(mesh, b, joint.children.back());
        }
    }
}

// ── RawAnimation construction ─────────────────────────────────────────────────

static void BuildRawAnimation(
    const SkinMesh& mesh, int clip_idx,
    const int* bone_to_ozz, int num_ozz_joints,
    ozz::animation::offline::RawAnimation& raw)
{
    raw.name     = mesh.ClipName(clip_idx);
    raw.duration = mesh.ClipDuration2(clip_idx);
    if (raw.duration < 0.001f) raw.duration = 0.001f;

    raw.tracks.resize(num_ozz_joints);

    for (int b = 0; b < mesh.bone_count; ++b) {
        int ozz_j = bone_to_ozz[b];
        if (ozz_j < 0 || ozz_j >= num_ozz_joints) continue;

        auto& track = raw.tracks[ozz_j];

        int kf_count = 0;
        const SkinKeyframe* kfs = mesh.GpuTrackKF(clip_idx, b, &kf_count);

        if (kfs && kf_count > 0) {
            track.translations.reserve(kf_count);
            track.rotations.reserve(kf_count);
            track.scales.reserve(kf_count);
            const float* bs = mesh.GpuBindS(b);
            for (int k = 0; k < kf_count; ++k) {
                const SkinKeyframe& kf = kfs[k];
                // RawAnimation uses SECONDS, not ratios.
                track.translations.push_back({kf.t, {kf.tx, kf.ty, kf.tz}});
                track.rotations.push_back   ({kf.t, {kf.qx, kf.qy, kf.qz, kf.qw}});
                if (bs) track.scales.push_back({kf.t, {bs[0], bs[1], bs[2]}});
            }
        } else {
            // No keyframes for this bone in this clip → bind pose.
            const float* bt = mesh.GpuBindT(b);
            const float* bq = mesh.GpuBindQ(b);
            const float* bs = mesh.GpuBindS(b);
            if (bt) track.translations.push_back({0.f, {bt[0], bt[1], bt[2]}});
            if (bq) track.rotations.push_back   ({0.f, {bq[0], bq[1], bq[2], bq[3]}});
            if (bs) track.scales.push_back       ({0.f, {bs[0], bs[1], bs[2]}});
        }
    }
}

// ── OzzAnimator::Init ─────────────────────────────────────────────────────────

bool OzzAnimator::Init(const SkinMesh& mesh) {
    bone_count_ = mesh.bone_count;
    if (bone_count_ <= 0) return false;

    memset(bone_to_ozz_, -1, sizeof(bone_to_ozz_));
    memset(ozz_to_bone_, -1, sizeof(ozz_to_bone_));

    // 1. Build RawSkeleton from flat bone array
    ozz::animation::offline::RawSkeleton raw_skel;
    for (int b = 0; b < bone_count_; ++b) {
        if (mesh.GpuParent(b) < 0) {
            raw_skel.roots.emplace_back();
            BuildJoint(mesh, b, raw_skel.roots.back());
        }
    }
    if (!raw_skel.Validate()) return false;

    // 2. Build runtime Skeleton
    {
        ozz::animation::offline::SkeletonBuilder builder;
        skel_ = builder(raw_skel);
        if (!skel_) return false;
    }

    // 3. Build joint index mapping by matching names
    {
        std::unordered_map<std::string, int> name_to_ozz;
        const auto& names = skel_->joint_names();
        for (int j = 0; j < skel_->num_joints(); ++j)
            name_to_ozz[names[j]] = j;

        for (int b = 0; b < bone_count_; ++b) {
            auto it = name_to_ozz.find(mesh.bone_names[b]);
            if (it != name_to_ozz.end()) {
                bone_to_ozz_[b]          = it->second;
                ozz_to_bone_[it->second] = b;
            }
        }
    }

    // 4. Copy inv-bind matrices
    for (int b = 0; b < bone_count_; ++b) {
        const float* ib = mesh.GpuInvBind(b);
        if (ib) memcpy(inv_bind_[b], ib, 64);
        else    mat4_identity(inv_bind_[b]);
    }

    // 5. Build animations (offline → runtime)
    {
        int nc = mesh.ClipCount();
        anims_.resize(nc);
        for (int c = 0; c < nc; ++c) {
            ozz::animation::offline::RawAnimation raw_anim;
            BuildRawAnimation(mesh, c, bone_to_ozz_, skel_->num_joints(), raw_anim);
            ozz::animation::offline::AnimationBuilder builder;
            anims_[c] = builder(raw_anim);
        }
    }

    // 6. Precompute SoA weight arrays for lower/upper body blend.
    // Standard lower-body mask: bones 1-11 (pelvis + legs in Kenshi GLB).
    {
        const int num_soa = skel_->num_soa_joints();
        upper_weights_soa_.resize(num_soa);
        lower_weights_soa_.resize(num_soa);

        for (int j = 0; j < skel_->num_joints(); ++j) {
            int our_bone  = ozz_to_bone_[j];
            bool is_lower = (our_bone >= 1 && our_bone <= 11);
            int  soa_slot = j / 4;
            int  lane     = j % 4;
            float* up = reinterpret_cast<float*>(&upper_weights_soa_[soa_slot]);
            float* lo = reinterpret_cast<float*>(&lower_weights_soa_[soa_slot]);
            up[lane] = is_lower ? 0.f : 1.f;
            lo[lane] = is_lower ? 1.f : 0.f;
        }
    }

    loaded_ = true;
    return true;
}

void OzzAnimator::Shutdown() {
    anims_.clear();
    skel_.reset();
    loaded_ = false;
}

float OzzAnimator::ClipDuration(int idx) const {
    if (idx < 0 || idx >= (int)anims_.size() || !anims_[idx]) return 0.f;
    return anims_[idx]->duration();
}

// ── Thread-local scratch (one per OS thread; safe for T2 async jobs) ─────────

struct OzzScratch {
    ozz::vector<ozz::math::SoaTransform>   locals_a;
    ozz::vector<ozz::math::SoaTransform>   locals_b;
    ozz::vector<ozz::math::SoaTransform>   blended;
    ozz::vector<ozz::math::Float4x4>       models;
    ozz::animation::SamplingJob::Context   ctx_a;
    ozz::animation::SamplingJob::Context   ctx_b;
    // Posture/slider-pose delta scratch (ApplyPostureDelta) — dedicated
    // context so re-sampling the posture clip twice per call doesn't thrash
    // ctx_a/ctx_b's cached cursor state for the base/over clips.
    ozz::vector<ozz::math::SoaTransform>   posture_neutral;
    ozz::vector<ozz::math::SoaTransform>   posture_current;
    ozz::animation::SamplingJob::Context   ctx_posture;
};
static thread_local OzzScratch g_scratch;

// ── ScaleSoATranslations ──────────────────────────────────────────────────────
// Scales per-bone local translations in SoaTransform before LocalToModel.
// Implements setBonePositionalSize (stretches limb chain without moving vertices).
// SoA layout: each SoaTransform holds 4 joints; bone ozz_j lives in lane ozz_j%4.

static void ScaleSoATranslations(
    ozz::vector<ozz::math::SoaTransform>& locals,
    const int* bone_to_ozz, int bone_count, int num_joints,
    const float (*pos_scales)[3])
{
    using namespace ozz::math;
    const int num_soa = (int)locals.size();
    for (int b = 0; b < bone_count; ++b) {
        const float* ps = pos_scales[b];
        if (ps[0] == 1.f && ps[1] == 1.f && ps[2] == 1.f) continue;
        const int ozz_j = bone_to_ozz[b];
        if (ozz_j < 0 || ozz_j >= num_joints) continue;
        const int soa_i = ozz_j / 4;
        const int lane  = ozz_j % 4;
        if (soa_i >= num_soa) continue;
        float x[4], y[4], z[4];
        StorePtrU(locals[soa_i].translation.x, x);
        StorePtrU(locals[soa_i].translation.y, y);
        StorePtrU(locals[soa_i].translation.z, z);
        x[lane] *= ps[0];
        y[lane] *= ps[1];
        z[lane] *= ps[2];
        locals[soa_i].translation.x = simd_float4::LoadPtrU(x);
        locals[soa_i].translation.y = simd_float4::LoadPtrU(y);
        locals[soa_i].translation.z = simd_float4::LoadPtrU(z);
    }
}

// ── ModelsToOutBones ──────────────────────────────────────────────────────────

void OzzAnimator::ModelsToOutBones(
    const ozz::span<const ozz::math::Float4x4> models,
    float* out_bones,
    float* out_model_xyz,
    const float (*bone_scales)[3]) const
{
    const int num_joints = (int)models.size();
    using namespace ozz::math;

    for (int b = 0; b < bone_count_; ++b) {
        const int ozz_j = bone_to_ozz_[b];
        float* dst = out_bones + b * 16;

        if (ozz_j < 0 || ozz_j >= num_joints) {
            mat4_identity(dst);
            continue;
        }

        const Float4x4& world = models[ozz_j];

        // Build inv_bind (optionally with per-bone scale: diag(sx,sy,sz) * inv_bind)
        Float4x4 inv;
        const float* ib = inv_bind_[b];
        inv.cols[0] = simd_float4::LoadPtrU(ib +  0);
        inv.cols[1] = simd_float4::LoadPtrU(ib +  4);
        inv.cols[2] = simd_float4::LoadPtrU(ib +  8);
        inv.cols[3] = simd_float4::LoadPtrU(ib + 12);

        if (bone_scales) {
            // Scale columns of inv_bind: diag(sx,sy,sz,1) * inv_bind
            // Column-major: scaling row i by bone_scales[b][i]
            // Each column of inv: col_j = (inv[0,j]*sx, inv[1,j]*sy, inv[2,j]*sz, inv[3,j])
            // We scale individual float rows — extract row components then reassemble.
            // Simpler: scale each column's xyz components in place.
            for (int col = 0; col < 4; ++col) {
                float c[4];
                StorePtrU(inv.cols[col], c);
                c[0] *= bone_scales[b][0];
                c[1] *= bone_scales[b][1];
                c[2] *= bone_scales[b][2];
                inv.cols[col] = simd_float4::LoadPtrU(c);
            }
        }

        const Float4x4 skin = world * inv;
        StorePtrU(skin.cols[0], dst +  0);
        StorePtrU(skin.cols[1], dst +  4);
        StorePtrU(skin.cols[2], dst +  8);
        StorePtrU(skin.cols[3], dst + 12);

        if (out_model_xyz) {
            float w[4];
            StorePtrU(world.cols[3], w);
            out_model_xyz[b*3+0] = w[0];
            out_model_xyz[b*3+1] = w[1];
            out_model_xyz[b*3+2] = w[2];
        }
    }
    for (int b = bone_count_; b < OZZ_ANIM_MAX_BONES; ++b)
        mat4_identity(out_bones + b * 16);
}

// ── OzzAnimator::ApplyPostureDelta ────────────────────────────────────────────
// Kenshi RE (editor_char_preview_sdlgpu.cpp's apply_slider): Posture/Shoulder-
// set/Neck-position sliders map linearly to a time position on a dedicated
// "postures"-style clip (setTimePosition(length * slider*0.01)). Composing
// the RAW pose at that time directly would bake in whatever arbitrary pose
// the clip happens to hold at the neutral slider value too — instead we
// compute a LOCAL rotation delta between the neutral-time pose and the
// current-time pose (conjugate(pose_neutral) * pose_current) and apply that
// delta on top of the base animation's already-sampled local rotation. This
// composes to an exact identity at the neutral slider value regardless of
// the clip's absolute pose there, and bones the clip never actually animates
// (single static keyframe) naturally produce an identity delta too, since
// sampling a constant track at any two ratios returns the same quaternion —
// no separate "which bones does this clip affect" mask needed.
void OzzAnimator::ApplyPostureDelta(
    ozz::vector<ozz::math::SoaTransform>& locals,
    int posture_clip, float t_neutral, float t_current) const
{
    if (posture_clip < 0 || posture_clip >= (int)anims_.size() || !anims_[posture_clip]) return;
    if (t_current == t_neutral) return;  // exact no-op — avoids 2 wasted SamplingJobs at default

    const ozz::animation::Animation* anim = anims_[posture_clip].get();
    const float dur = anim->duration();
    const float ratio_n = (dur > 0.05f) ? fmodf(t_neutral,  dur) / dur : 0.f;
    const float ratio_c = (dur > 0.05f) ? fmodf(t_current, dur) / dur : 0.f;

    OzzScratch& sc = g_scratch;
    sc.posture_neutral.resize(skel_->num_soa_joints());
    sc.posture_current.resize(skel_->num_soa_joints());

    if (sc.ctx_posture.max_tracks() < anim->num_tracks())
        sc.ctx_posture.Resize(anim->num_tracks());

    {
        ozz::animation::SamplingJob sj;
        sj.animation = anim;
        sj.context   = &sc.ctx_posture;
        sj.ratio     = ratio_n;
        sj.output    = ozz::make_span(sc.posture_neutral);
        if (!sj.Run()) return;
    }
    {
        ozz::animation::SamplingJob sj;
        sj.animation = anim;
        sj.context   = &sc.ctx_posture;  // same clip's context — re-sample at a new ratio, standard scrub usage
        sj.ratio     = ratio_c;
        sj.output    = ozz::make_span(sc.posture_current);
        if (!sj.Run()) return;
    }

    using namespace ozz::math;
    const int num_soa = (int)locals.size();
    for (int slot = 0; slot < num_soa && slot < (int)sc.posture_neutral.size(); ++slot) {
        const SoaQuaternion delta = Conjugate(sc.posture_neutral[slot].rotation)
                                   * sc.posture_current[slot].rotation;
        locals[slot].rotation = locals[slot].rotation * delta;
    }
}

// ── OzzAnimator::Sample ───────────────────────────────────────────────────────

void OzzAnimator::Sample(int clip_idx, float time_s,
                         float* out_bones,
                         float* out_model_xyz,
                         const float (*bone_scales)[3],
                         const float (*pos_scales)[3],
                         int posture_clip, float posture_t_neutral,
                         float posture_t_current) const {
    if (!loaded_ || clip_idx < 0 || clip_idx >= (int)anims_.size()
                 || !anims_[clip_idx]) {
        for (int b = 0; b < OZZ_ANIM_MAX_BONES; ++b) mat4_identity(out_bones + b*16);
        return;
    }

    const ozz::animation::Animation* anim = anims_[clip_idx].get();
    const float dur   = anim->duration();
    // Clips < 50ms are single-frame reference poses — always sample frame 0.
    // Matches old GetFinalBones() threshold to avoid 30 Hz oscillation on idle_stand_normal.
    const float ratio = (dur > 0.05f) ? fmodf(time_s, dur) / dur : 0.f;

    OzzScratch& sc = g_scratch;
    sc.locals_a.resize(skel_->num_soa_joints());
    sc.models.resize(skel_->num_joints());

    if (sc.ctx_a.max_tracks() < anim->num_tracks())
        sc.ctx_a.Resize(anim->num_tracks());

    ozz::animation::SamplingJob sj;
    sj.animation = anim;
    sj.context   = &sc.ctx_a;
    sj.ratio     = ratio;
    sj.output    = ozz::make_span(sc.locals_a);
    if (!sj.Run()) {
        for (int b = 0; b < OZZ_ANIM_MAX_BONES; ++b) mat4_identity(out_bones + b*16);
        return;
    }

    ApplyPostureDelta(sc.locals_a, posture_clip, posture_t_neutral, posture_t_current);

    if (pos_scales)
        ScaleSoATranslations(sc.locals_a, bone_to_ozz_, bone_count_,
                             skel_->num_joints(), pos_scales);

    ozz::animation::LocalToModelJob l2m;
    l2m.skeleton = skel_.get();
    l2m.input    = ozz::make_span(sc.locals_a);
    l2m.output   = ozz::make_span(sc.models);
    if (!l2m.Run()) {
        for (int b = 0; b < OZZ_ANIM_MAX_BONES; ++b) mat4_identity(out_bones + b*16);
        return;
    }

    ModelsToOutBones(ozz::make_span(sc.models), out_bones, out_model_xyz, bone_scales);
}

// ── OzzAnimator::Blend ────────────────────────────────────────────────────────

void OzzAnimator::Blend(int base_clip, float base_t,
                        int over_clip,  float over_t,
                        const bool* /*lower_mask*/, float* out_bones,
                        const float (*bone_scales)[3],
                        const float (*pos_scales)[3],
                        int posture_clip, float posture_t_neutral,
                        float posture_t_current) const {
    if (!loaded_) {
        for (int b = 0; b < OZZ_ANIM_MAX_BONES; ++b) mat4_identity(out_bones + b*16);
        return;
    }
    if (base_clip < 0 || base_clip >= (int)anims_.size() || !anims_[base_clip]) {
        Sample(over_clip, over_t, out_bones, nullptr, bone_scales, pos_scales,
               posture_clip, posture_t_neutral, posture_t_current);
        return;
    }
    if (over_clip < 0 || over_clip >= (int)anims_.size() || !anims_[over_clip]) {
        Sample(base_clip, base_t, out_bones, nullptr, bone_scales, pos_scales,
               posture_clip, posture_t_neutral, posture_t_current);
        return;
    }

    const ozz::animation::Animation* anim_base = anims_[base_clip].get();
    const ozz::animation::Animation* anim_over = anims_[over_clip].get();

    auto to_ratio = [](const ozz::animation::Animation* a, float t) {
        const float d = a->duration();
        return (d > 0.05f) ? fmodf(t, d) / d : 0.f;
    };

    OzzScratch& sc = g_scratch;
    sc.locals_a.resize(skel_->num_soa_joints());
    sc.locals_b.resize(skel_->num_soa_joints());
    sc.blended.resize(skel_->num_soa_joints());
    sc.models.resize(skel_->num_joints());

    if (sc.ctx_a.max_tracks() < anim_base->num_tracks())
        sc.ctx_a.Resize(anim_base->num_tracks());
    if (sc.ctx_b.max_tracks() < anim_over->num_tracks())
        sc.ctx_b.Resize(anim_over->num_tracks());

    {
        ozz::animation::SamplingJob sj;
        sj.animation = anim_base;
        sj.context   = &sc.ctx_a;
        sj.ratio     = to_ratio(anim_base, base_t);
        sj.output    = ozz::make_span(sc.locals_a);
        if (!sj.Run()) { Sample(base_clip, base_t, out_bones); return; }
    }
    {
        ozz::animation::SamplingJob sj;
        sj.animation = anim_over;
        sj.context   = &sc.ctx_b;
        sj.ratio     = to_ratio(anim_over, over_t);
        sj.output    = ozz::make_span(sc.locals_b);
        if (!sj.Run()) { Sample(base_clip, base_t, out_bones); return; }
    }

    // Blend: upper body = base_clip, lower body = over_clip.
    ozz::animation::BlendingJob::Layer layers[2];
    layers[0].transform     = ozz::make_span(sc.locals_a);
    layers[0].weight        = 1.f;
    layers[0].joint_weights = ozz::make_span(upper_weights_soa_);
    layers[1].transform     = ozz::make_span(sc.locals_b);
    layers[1].weight        = 1.f;
    layers[1].joint_weights = ozz::make_span(lower_weights_soa_);

    ozz::animation::BlendingJob bj;
    bj.threshold  = .1f;
    bj.rest_pose  = skel_->joint_rest_poses();
    bj.layers     = ozz::make_span(layers);
    bj.output     = ozz::make_span(sc.blended);
    if (!bj.Run()) { Sample(base_clip, base_t, out_bones); return; }

    ApplyPostureDelta(sc.blended, posture_clip, posture_t_neutral, posture_t_current);

    if (pos_scales)
        ScaleSoATranslations(sc.blended, bone_to_ozz_, bone_count_,
                             skel_->num_joints(), pos_scales);

    ozz::animation::LocalToModelJob l2m;
    l2m.skeleton = skel_.get();
    l2m.input    = ozz::make_span(sc.blended);
    l2m.output   = ozz::make_span(sc.models);
    if (!l2m.Run()) { Sample(base_clip, base_t, out_bones); return; }

    ModelsToOutBones(ozz::make_span(sc.models), out_bones, nullptr, bone_scales);
}

// ── OzzAnimator::SampleWorldMats ─────────────────────────────────────────────

void OzzAnimator::SampleWorldMats(int clip_idx, float time_s,
                                   float (*out_world)[16]) const {
    for (int b = 0; b < bone_count_; ++b) mat4_identity(out_world[b]);
    if (!loaded_ || clip_idx < 0 || clip_idx >= (int)anims_.size()
                 || !anims_[clip_idx]) return;

    const ozz::animation::Animation* anim = anims_[clip_idx].get();
    const float dur   = anim->duration();
    const float ratio = (dur > 0.05f) ? fmodf(time_s, dur) / dur : 0.f;

    OzzScratch& sc = g_scratch;
    sc.locals_a.resize(skel_->num_soa_joints());
    sc.models.resize(skel_->num_joints());

    if (sc.ctx_a.max_tracks() < anim->num_tracks())
        sc.ctx_a.Resize(anim->num_tracks());

    ozz::animation::SamplingJob sj;
    sj.animation = anim;
    sj.context   = &sc.ctx_a;
    sj.ratio     = ratio;
    sj.output    = ozz::make_span(sc.locals_a);
    if (!sj.Run()) return;

    ozz::animation::LocalToModelJob l2m;
    l2m.skeleton = skel_.get();
    l2m.input    = ozz::make_span(sc.locals_a);
    l2m.output   = ozz::make_span(sc.models);
    if (!l2m.Run()) return;

    using namespace ozz::math;
    const int num_joints = skel_->num_joints();
    for (int b = 0; b < bone_count_; ++b) {
        const int ozz_j = bone_to_ozz_[b];
        if (ozz_j < 0 || ozz_j >= num_joints) continue;
        StorePtrU(sc.models[ozz_j].cols[0], out_world[b] +  0);
        StorePtrU(sc.models[ozz_j].cols[1], out_world[b] +  4);
        StorePtrU(sc.models[ozz_j].cols[2], out_world[b] +  8);
        StorePtrU(sc.models[ozz_j].cols[3], out_world[b] + 12);
    }
}

// ── OzzAnimator::BlendAdditive ────────────────────────────────────────────────

void OzzAnimator::BlendAdditive(float* out_bones,
                                int add_clip, float add_t,
                                float weight) const {
    if (!loaded_ || weight < 0.001f) return;
    if (add_clip < 0 || add_clip >= (int)anims_.size() || !anims_[add_clip]) return;

    OzzScratch& sc = g_scratch;
    sc.locals_a.resize(skel_->num_soa_joints());
    sc.models.resize(skel_->num_joints());

    const ozz::animation::Animation* anim = anims_[add_clip].get();
    // Use ctx_b so we don't corrupt ctx_a which holds the base animation's cache.
    if (sc.ctx_b.max_tracks() < anim->num_tracks())
        sc.ctx_b.Resize(anim->num_tracks());

    const float dur_add = anim->duration();
    const float ratio = (dur_add > 0.05f) ? fmodf(add_t, dur_add) / dur_add : 0.f;

    ozz::animation::SamplingJob sj;
    sj.animation = anim;
    sj.context   = &sc.ctx_b;
    sj.ratio     = ratio;
    sj.output    = ozz::make_span(sc.locals_a);
    if (!sj.Run()) return;

    ozz::animation::LocalToModelJob l2m;
    l2m.skeleton = skel_.get();
    l2m.input    = ozz::make_span(sc.locals_a);
    l2m.output   = ozz::make_span(sc.models);
    if (!l2m.Run()) return;

    using namespace ozz::math;
    const int num_joints = (int)sc.models.size();
    const SimdFloat4 w = simd_float4::Load1(weight);

    for (int b = 0; b < bone_count_; ++b) {
        const int ozz_j = bone_to_ozz_[b];
        if (ozz_j < 0 || ozz_j >= num_joints) continue;

        Float4x4 inv;
        const float* ib = inv_bind_[b];
        inv.cols[0] = simd_float4::LoadPtrU(ib +  0);
        inv.cols[1] = simd_float4::LoadPtrU(ib +  4);
        inv.cols[2] = simd_float4::LoadPtrU(ib +  8);
        inv.cols[3] = simd_float4::LoadPtrU(ib + 12);

        const Float4x4 add_skin = sc.models[ozz_j] * inv;
        float* dst = out_bones + b * 16;

        for (int col = 0; col < 4; ++col) {
            const SimdFloat4 base_col = simd_float4::LoadPtrU(dst + col * 4);
            // additive: base + (add - base) * weight  = lerp(base, add, w)
            const SimdFloat4 result = MAdd(w, add_skin.cols[col] - base_col, base_col);
            StorePtrU(result, dst + col * 4);
        }
    }
}

#pragma once
// SkinMesh — loads a skinned GLB (POSITION+NORMAL+JOINTS_0+WEIGHTS_0+skin+animations).
// Vertex stride = 44: pos(12) + norm(12) + joints_u8x4(4) + weights_f4(16).
// Animation clips are stored CPU-side; GetFinalBones() produces mat4[MAX_SKIN_BONES].

#include <monkey_dust/render/gpu_static_buffer.h>
#include <cstdint>
#include <cstring>
#include <cmath>

static constexpr int MAX_SKIN_BONES   = 64;  // must match animation_soa MAX_BONES (SSBO stride)
static constexpr int MAX_SKIN_CLIPS   = 16;
static constexpr int MAX_SKIN_KF      = 512; // keyframes per track
static constexpr int MAX_MORPH_TARGETS = 32; // blend shapes per mesh

struct SkinVertex {
    float    x, y, z;      // position  (offset  0, 12 B)
    float    nx, ny, nz;   // normal    (offset 12, 12 B)
    float    u, v;         // texcoord  (offset 24,  8 B)
    uint8_t  j[4];         // joints    (offset 32,  4 B)
    float    w[4];         // weights   (offset 36, 16 B)
};
static_assert(sizeof(SkinVertex) == 52, "SkinVertex stride");

struct SkinKeyframe {
    float t;
    float tx, ty, tz;
    float qx, qy, qz, qw;
};

struct SkinTrack {
    SkinKeyframe kf[MAX_SKIN_KF];
    int          count = 0;
};

// VBfA-R: animation clip metadata flags
namespace AnimFlags {
    constexpr uint32_t LOOPING       = 1u <<  0;  // clip loops continuously
    constexpr uint32_t ROOT_MOTION   = 1u <<  1;  // apply root bone translation
    constexpr uint32_t ADDITIVE      = 1u <<  2;  // additive layer blend
    constexpr uint32_t MIRROR        = 1u <<  3;  // mirror left/right bones
    constexpr uint32_t NO_IK         = 1u <<  4;  // skip foot IK for this clip
    constexpr uint32_t UPPER_ONLY    = 1u <<  5;  // drive upper body only
    constexpr uint32_t LOWER_ONLY    = 1u <<  6;  // drive lower body only
    constexpr uint32_t IDLE_VAR      = 1u <<  7;  // idle variation (random pick)
    constexpr uint32_t COMBAT        = 1u <<  8;  // combat-mode clip
    constexpr uint32_t DEATH         = 1u <<  9;  // death/ragdoll lead-in
    constexpr uint32_t STAGGER       = 1u << 10;  // stagger / hit-react
    constexpr uint32_t WEAPON_DRAW   = 1u << 11;  // weapon draw/sheathe
    constexpr uint32_t INTERACT      = 1u << 12;  // interaction (loot/use)
    constexpr uint32_t SNEAK         = 1u << 13;  // sneak/crouch locomotion
    constexpr uint32_t SPRINT        = 1u << 14;  // sprint
    constexpr uint32_t SWIM          = 1u << 15;  // swimming
    constexpr uint32_t CLIMB         = 1u << 16;  // climbing
    constexpr uint32_t TURN_L        = 1u << 17;  // turn in place left
    constexpr uint32_t TURN_R        = 1u << 18;  // turn in place right
    constexpr uint32_t FOOTSTEP_L    = 1u << 19;  // left foot contact marker
    constexpr uint32_t FOOTSTEP_R    = 1u << 20;  // right foot contact marker
    constexpr uint32_t ATTACK_SWING  = 1u << 21;  // weapon swing arc
    constexpr uint32_t ATTACK_THRUST = 1u << 22;  // weapon thrust
    constexpr uint32_t BLOCK         = 1u << 23;  // blocking pose
    // VBfA RE-confirmed (viking.exe.c Level 1 analysis):
    constexpr uint32_t BLEND_OUT     = 1u << 24;  // explicit blend-out phase
    constexpr uint32_t KNOCKDOWN     = 1u << 25;  // ragdoll entry / stun drop
    constexpr uint32_t GETUP         = 1u << 26;  // recover from knockdown
    constexpr uint32_t PAIN          = 1u << 27;  // pain flinch reaction
    constexpr uint32_t LEDGE_GRAB    = 1u << 28;  // ledge/climb grab animation
    constexpr uint32_t THROW         = 1u << 29;  // throw/toss action
    constexpr uint32_t COUNTER       = 1u << 30;  // parry counter-attack
    constexpr uint32_t TWEEN         = 1u << 31;  // transition tween (N blend frames)
    // Extended flags (64-bit if needed later): BOSS, NO_CHECKS map to bits 0-1 of high word
}

struct SkinClip {
    char      name[48]   = {};
    float     duration   = 0.f;
    uint32_t  flags      = 0u;    // VBfA-R: bitmask of AnimFlags constants
    float     impact_time= 0.f;  // VBfA-R: time (s) at which hit/impact fires
    SkinTrack tracks[MAX_SKIN_BONES];
    int       bone_count = 0;
};

// Forward declaration so GetFinalBonesScaled can be declared without
// creating a circular include with char_customization.h.
struct CharScales;

class SkinMesh {
public:
    bool LoadGLB(const char* path);
    void Shutdown();

    // Compute mat4[MAX_SKIN_BONES] for given clip + time.
    // out_bones must point to at least MAX_SKIN_BONES * 16 floats.
    void GetFinalBones(int clip_idx, float time_s, float* out_bones) const;

    // Like GetFinalBones but applies Kenshi-style bone+pos scales per-bone.
    // scales.bone[i] = vertex scale (setBoneSize), scales.pos[i] = translation scale.
    // Requires #include <monkey_dust/render/char_customization.h> for CharScales definition.
    void GetFinalBonesScaled(int clip_idx, float time_s,
                              const CharScales& scales, float* out) const;

    // Same loop, but also outputs model-space bone positions (xyz, 3 floats per bone)
    // and optionally the pre-invbind world matrices (float[MAX_SKIN_BONES * 16]).
    // out_world_mats: required by FootIK::SolveLeg — pass s_ik_worlds[MAX_SKIN_BONES*16].
    void GetFinalBonesFull(int clip_idx, float time_s,
                           float* out_bones,
                           float* out_model_xyz,
                           float* out_world_mats = nullptr) const;

    // PERF-18: LOD-simplified bone eval — only evaluates first max_bones bones.
    // VBfA RE: 3 LOD tiers per character. T2 (80-150m) needs ~12 bones.
    //   12 = root + pelvis + spine×2 + head + shoulder×2 + hip×2 + knee×2
    // Upper bones (index ≥ max_bones) fall back to bind pose — invisible at distance.
    // Out: out_bones[MAX_SKIN_BONES * 16] (full stride; upper range is bind-pose).
    // Default max_bones=12 matches T2 skeleton LOD recommendation.
    void GetFinalBonesLOD(int clip_idx, float time_s,
                          float* out_bones, int max_bones = 12) const;

    // Patch one skinning matrix after IK correction:
    //   out_bones[bone_idx*16] = new_world_16 * inv_bind_[bone_idx]
    void PatchBoneIK(int bone_idx, const float* new_world_16,
                     float* out_bones) const;

    // ── Bind-pose/track accessors — used by OzzAnimator to build its runtime skeleton ────
    // Returns pointer to track keyframes; *out_count = number of keyframes (0 if no track).
    const SkinKeyframe* GpuTrackKF(int clip_idx, int bone_idx, int* out_count) const {
        if (clip_idx<0||clip_idx>=clip_count_||bone_idx<0||bone_idx>=bone_count) { *out_count=0; return nullptr; }
        const SkinTrack& tr = clips_[clip_idx].tracks[bone_idx];
        *out_count = tr.count;
        return tr.count > 0 ? tr.kf : nullptr;
    }
    float ClipDuration2(int i) const { return (i>=0&&i<clip_count_)?clips_[i].duration:0.f; }
    const float* GpuInvBind(int b)      const { return (b>=0&&b<bone_count)?inv_bind_[b]:nullptr; }
    int          GpuParent(int b)        const { return (b>=0&&b<bone_count)?parent_[b]:-1; }
    int          GpuProcessOrder(int i) const { return (i>=0&&i<bone_count)?process_order_[i]:-1; }
    const float* GpuBindT(int b)        const { return (b>=0&&b<bone_count)?bind_t_[b]:nullptr; }
    const float* GpuBindQ(int b)        const { return (b>=0&&b<bone_count)?bind_q_[b]:nullptr; }
    const float* GpuBindS(int b)        const { return (b>=0&&b<bone_count)?bind_s_[b]:nullptr; }

    int ClipCount()            const { return clip_count_; }
    int ClipIndexByName(const char* name) const;

    // KEN-MORPH-1: bone-name lookup by MurmurHash3_x86_32 (seed=0x3a8efa67).
    // FindBoneByName returns index or -1; hashes precomputed in LoadGLB.
    static uint32_t BoneHash(const char* name);
    int FindBoneByName(const char* name) const;
    const char* ClipName(int i) const { return (i>=0&&i<clip_count_)?clips_[i].name:""; }
    float ClipDuration(int i)  const { return (i>=0&&i<clip_count_)?clips_[i].duration:0.f; }

    // Apply inverse bind matrices: bones[i] = bones[i] * inv_bind_[i].
    // Call after GetFinalBones*() to get correct GPU skinning matrices.
    void ApplyInvBind(float* bones) const;

    // Returns the world-space Y of bone_idx from the last GetFinalBones*() call.
    // Used for foot-grounding queries without a full re-evaluation.
    float LastBoneWorldY(int bone_idx) const;

    // Upper/lower body blend: evaluate base_clip for all bones, then override lower-body
    // bones from override_clip (use lower_mask[i]=true for bones to override).
    // Pass lower_mask=nullptr to skip override (equivalent to plain GetFinalBones).
    // out_world_mats: optional float[MAX_SKIN_BONES*16] — same world mats used internally,
    //   required by FootIK::SolveLeg so IK uses the same matrices as the skinning output.
    void GetFinalBonesBlend(int base_clip, float base_t,
                            int override_clip, float override_t,
                            const bool* lower_mask, float* out,
                            float* out_world_mats = nullptr) const;

    // CPU vertex data — valid until next LoadGLB() call (points to internal static buffer).
    const SkinVertex* CpuVerts()  const { return cpu_verts_; }
    uint32_t          VertCount() const { return vert_count_; }

    // Blend shape (morph target) interface.
    // morph_weights[i] is public — set by caller in [0..1].
    // WriteMorphedVerts() copies cpu_verts_ and applies weighted deltas into out[].
    // out must hold at least vert_count_ SkinVertex structs.
    int         MorphCount()           const { return morph_count_; }
    const char* MorphName(int i)       const { return (i>=0&&i<morph_count_)?morph_names_[i]:""; }
    void        WriteMorphedVerts(SkinVertex* out) const;

    float morph_weights[MAX_MORPH_TARGETS] = {};  // set by caller; used by WriteMorphedVerts

    GpuStaticBuffer vbo;
    GpuStaticBuffer ibo;
    uint32_t        index_count  = 0;
    uint32_t        vert_count_  = 0;
    bool            loaded       = false;
    bool            indices_u16  = true;
    int             bone_count   = 0;
    char            bone_names[MAX_SKIN_BONES][32] = {};

private:
    const SkinVertex* cpu_verts_ = nullptr;
    // Inverse bind matrices per bone (mat4, column-major)
    float inv_bind_[MAX_SKIN_BONES][16] = {};
    // Parent index per bone (-1 = root)
    int   parent_[MAX_SKIN_BONES]  = {};
    // Bind-pose local TRS per bone
    float bind_t_[MAX_SKIN_BONES][3] = {};
    float bind_q_[MAX_SKIN_BONES][4] = {};
    float bind_s_[MAX_SKIN_BONES][3] = {};
    // Topological processing order (parent always before child)
    int   process_order_[MAX_SKIN_BONES] = {};
    // KEN-MORPH-1: bone name hashes precomputed in LoadGLB for O(1) lookup
    uint32_t bone_hash_[MAX_SKIN_BONES] = {};

    SkinClip clips_[MAX_SKIN_CLIPS];
    int      clip_count_ = 0;

    // Morph target data (loaded from GLB prim->targets)
    int   morph_count_  = 0;
    char  morph_names_[MAX_MORPH_TARGETS][48] = {};
    // Heap-allocated: morph_count_ × vert_count_ × 3 floats (pos deltas)
    // Layout: delta[m][v] at index (m * vert_count_ + v) * 3
    float* morph_deltas_ = nullptr;

    // Helpers
    static void mat4_identity(float* m);
    static void mat4_from_trs(float* m, const float* t, const float* q, const float* s);
    static void mat4_mul(float* out, const float* a, const float* b);
    static void quat_lerp(float* out, const float* a, const float* b, float t);
    static void lerp3(float* out, const float* a, const float* b, float t);
    static float slerp_t(const SkinTrack& tr, float time_s,
                         float* out_t, float* out_q);
};

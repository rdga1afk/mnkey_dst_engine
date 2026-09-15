#pragma once
#include <monkey_dust/platform/md_hints.h>
#include <monkey_dust/render/ssbo.h>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

static constexpr int MAX_BONES        = 64;  // SSBO stride fixed at 64 for SDL_GPU transfer alignment; Kenshi uses 30 of 64
static constexpr int MAX_ANIMATED_NPC = 500;
static constexpr int MAX_ANIM_CLIPS   = 8;

// VBfA lines 9042-9043: hard cap on draw calls per renderer stream.
// 2 streams × 200 objects = 400 max draw calls/frame.
static constexpr int MAX_DRAW_CALLS_PER_STREAM = 200;
static constexpr int MAX_DRAW_STREAMS          = 2;
static constexpr int MAX_DRAW_CALLS_TOTAL      = MAX_DRAW_CALLS_PER_STREAM * MAX_DRAW_STREAMS;

// ── Kenshi RE animation blend constants (RE 2026-06-25, HIGH confidence, 6+ batches) ──
// Clip blend-out
static constexpr float ANIM_BLEND_OUT_START     = 0.96f;  // start fade at 96% of clip_length
// Manual-control weight ramp (e.g. player taking control from AI)
static constexpr float ANIM_MANUAL_INIT_WEIGHT  = 0.05f;  // initial weight when entering manual
static constexpr float ANIM_MANUAL_WEIGHT_RAMP  = 0.01f;  // weight increment per logic tick (10Hz → ~10s to full)
// Ragdoll→anim recovery
static constexpr float ANIM_RAGDOLL_WEIGHT      = 0.05f;  // initial blend weight on recovery
static constexpr float ANIM_RAGDOLL_SPEED       = 0.01f;  // blend speed per tick
static constexpr float ANIM_RAGDOLL_WINDOW      = 5.0f;   // max recovery window in seconds
static constexpr float ANIM_RAGDOLL_SEC_WEIGHT  = 0.4f;   // secondary anim weight during recovery
// Locomotion playback speed clamp
static constexpr float ANIM_LOCO_SPEED_MIN      = 0.4f;   // min playback speed (slow walk)
static constexpr float ANIM_LOCO_SPEED_MAX      = 1.0f;   // max playback speed (run)
// Sync window for finisher/combo moves
static constexpr float ANIM_SYNC_WINDOW         = 0.1f;   // accept sync if within 0.1s of sync_time

// Per-NPC animation state — 16 bytes.
struct AnimNpcState {
    uint32_t slot;
    uint32_t clip_id;   // 0=IDLE, 1=WALK, 2=ATTACK
    float    time_s;
    uint32_t lod_tier;  // 0-2: full-rate eval; 3-4: skip (stale pose)
};
static_assert(sizeof(AnimNpcState) == 16, "AnimNpcState size mismatch");

// ── AnimEvent ─────────────────────────────────────────────────────────────────
// Per-clip keyframe events (RE: VBfA bakes FRAME_OF_WEAPON_GRAB, RIGHT_FOOT_DOWN,
// ATTACK_OFFSET_*, FRAME_DEATH etc. directly into animation metadata).
// 4 bytes each → 8 events per clip = 32 bytes overhead per clip.
enum class AnimEventType : uint8_t {
    None          = 0,
    WeaponGrab    = 1,  // FRAME_OF_WEAPON_GRAB — attach weapon to hand bone
    WeaponRelease = 2,  // FRAME_OF_WEAPON_RELEASE — detach weapon
    Footstep      = 3,  // RIGHT FOOT DOWN — trigger footstep audio
    AttackHit     = 4,  // ATTACK_OFFSET_* — param: 0=centre,1=left,2=right,3=above
    Death         = 5,  // FRAME_DEATH — spawn ragdoll, disable nav
    VfxSpawn      = 6,  // ANIM_METADATA_SPECIAL_EFFECT_S — param: vfx type index
    BlendSync     = 7,  // FRAME_CONVERGE — sync point for cross-fade blends
};

struct AnimEvent {
    uint16_t      frame;  // keyframe index where event fires
    AnimEventType type;
    int8_t        param;  // AttackHit: direction; VfxSpawn: vfx index; others: 0
};
static_assert(sizeof(AnimEvent) == 4, "AnimEvent must be 4 bytes");
static constexpr int MAX_EVENTS_PER_CLIP = 8;

struct AnimationClip {
    char      name[32];
    uint8_t   id;
    float     duration_s;
    int       frame_count;
    uint8_t   event_count;                       // number of valid entries in events[]
    uint8_t   _pad[3];
    AnimEvent events[MAX_EVENTS_PER_CLIP];       // 32 bytes — keyframe-triggered events
};

// ─────────────────────────────────────────────────────────
// AnimationSoA — singleton: per-NPC anim state + GPU SSBOs.
//
// SSBO layout:
//   binding 4: mat4[MAX_ANIMATED_NPC × MAX_BONES]  ← written by CPU LBS (OzzAnimator), read by animated.vert
//
// Usage:
//   Init()               — once after window open
//   LoadClipsFromFile()  — loads animation clips JSON
//   SetClip(slot, id)    — per logic tick, per NPC
//   Advance(dt)          — per logic tick
//   Upload()             — per render frame, binds SSBOs
//   BindSSBOs()          — re-bind after other draws may rebind
//   Shutdown()           — on exit
// ─────────────────────────────────────────────────────────

class AnimationSoA {
public:
    static AnimationSoA& Get() {
        static AnimationSoA inst;
        return inst;
    }

    void Init() {
        memset(states_, 0, sizeof(states_));
        for (int i = 0; i < MAX_ANIMATED_NPC; ++i)
            states_[i].slot = (uint32_t)i;
        // mat4 = 64 bytes (16 floats); finalBones: MAX_ANIMATED_NPC * MAX_BONES * 64
        // Written by CPU LBS (OzzAnimator::Sample); read by animated.vert directly as mat4.
        bones_ssbo_.Init(MAX_ANIMATED_NPC * MAX_BONES * 64,
                         SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ);
        LoadDefaults();
    }

    int LoadClipsFromFile(const char* path) {
        FILE* f = fopen(path, "rb");
        if (!f) return clips_count_;
        static char buf[2048];
        int n = (int)fread(buf, 1, sizeof(buf) - 1, f);
        fclose(f);
        buf[n] = '\0';
        clips_count_ = 0;
        const char* cur = buf;
        while (clips_count_ < MAX_ANIM_CLIPS) {
            const char* p = strstr(cur, "\"id\"");
            if (!p) break;
            AnimationClip& c = clips_[clips_count_];
            memset(&c, 0, sizeof(c));
            int id_v = 0, fc = 24;
            float dur = 1.0f;
            ParseInt(p, "\"id\"",       id_v);
            ParseFloat(p, "\"duration\"", dur);
            ParseInt(p, "\"frames\"",   fc);
            ParseStr(p, "\"name\"",     c.name, 32);
            c.id = (uint8_t)id_v;
            c.duration_s  = dur;
            c.frame_count = fc;
            clips_count_++;
            cur = p + 4;
        }
        if (clips_count_ == 0) LoadDefaults();
        return clips_count_;
    }

    void SetClip(int slot, uint8_t clip_id) {
        if (MD_UNLIKELY(slot < 0 || slot >= MAX_ANIMATED_NPC)) return;
        if (states_[slot].clip_id == clip_id) return;

        // KEN-ANIM-1: sync window — if old clip is near its end (within ANIM_SYNC_WINDOW),
        // start new clip at the same fractional phase to avoid a pop at the loop point.
        float new_t = 0.0f;
        uint32_t old_cid = states_[slot].clip_id;
        float    old_t   = states_[slot].time_s;
        if (old_cid < (uint32_t)clips_count_ && clip_id < (uint8_t)clips_count_) {
            float old_dur = clips_[old_cid].duration_s;
            float new_dur = clips_[clip_id].duration_s;
            if (old_dur > 0.f && new_dur > 0.f &&
                (old_dur - old_t) <= ANIM_SYNC_WINDOW) {
                new_t = (old_t / old_dur) * new_dur;
            }
        }

        states_[slot].clip_id = clip_id;
        states_[slot].time_s  = new_t;
    }

    void Advance(float dt) {
        // PERF-16: stagger animation time advancement by lod_tier.
        // Kenshi RE: entity_id % 500 / % 252 spreads blend-eval cost across frames.
        // T0/T1 (lod 0-1): advance every frame.
        // T2    (lod 2):   advance every 2 frames (slot % 2 == frame_counter_ % 2).
        // T3+   (lod 3-4): skip entirely (static pose / offscreen).
        const uint32_t fc = frame_counter_;
        for (int i = 0; i < MAX_ANIMATED_NPC; ++i) {
            const uint32_t tier = states_[i].lod_tier;
            if (tier >= 3) continue;                           // T3+: no update
            if (tier == 2 && ((uint32_t)i % 2u != fc % 2u)) continue; // T2: every 2nd frame
            uint32_t cid = states_[i].clip_id;
            float dur = (cid < (uint32_t)clips_count_) ? clips_[cid].duration_s : 1.0f;
            states_[i].time_s += dt;
            // Kenshi RE progress wrap: >2.0 → hard reset; >1.0 → floor-wrap.
            float prog = (dur > 0.f) ? states_[i].time_s / dur : 0.f;
            if (prog > 2.0f) states_[i].time_s = 0.f;
            else if (states_[i].time_s >= dur) states_[i].time_s -= dur;
        }
    }

    // Call once per frame after all draw/compute that reads GPU anim data.
    void AdvanceFrame() { ++frame_counter_; }

#ifdef MD_SDL_GPU
    SDL_GPUBuffer* SDLBonesBuffer()    const { return bones_ssbo_.SDLBuffer(); }
    // CPU-side bone matrices upload (real-mesh animation path — OzzAnimator::Sample/Blend
    // writes s_all_bones in npc_render_frame_prep.cpp, this uploads it each frame).
    // data: float[npc_count * MAX_BONES * 16], laid out at slot offsets 0..npc_count-1
    void UploadBonesInCmd(md::GpuCommandBufferHandle cmd, const void* data, int bytes, int byte_offset = 0) {
        if (bones_ssbo_.SDLBuffer()) bones_ssbo_.UploadInCmd(cmd, data, bytes, byte_offset);
    }
#endif

    // KEN-MORPH-1 (setBoneSize equivalent) is live via the CPU path:
    // CharBodyState::bone_scales/pos_scales, consumed directly by OzzAnimator::Sample/Blend.

    void Shutdown() {
        bones_ssbo_.Shutdown();
    }

    // Accessors for editor panels (replaces direct field access — БОРГ-7/8).
    AnimNpcState&       GetState(int slot)       { return states_[slot]; }
    const AnimNpcState& GetState(int slot) const { return states_[slot]; }
    int                 ClipCount()        const { return clips_count_; }
    const AnimationClip& GetClip(int i)   const { return clips_[i]; }

private:
    AnimationSoA() = default;

    alignas(64) AnimNpcState  states_[MAX_ANIMATED_NPC];
    AnimationClip clips_[MAX_ANIM_CLIPS];
    int           clips_count_ = 0;

    SSBO               bones_ssbo_;      // CPU LBS output (OzzAnimator) — regular SSBO
    uint32_t           frame_counter_ = 0; // PERF-16: stagger index

    void LoadDefaults() {
        clips_count_ = 3;
        auto set = [&](int i, const char* n, uint8_t id, float dur, int frames) {
            strncpy(clips_[i].name, n, 31);
            clips_[i].id = id; clips_[i].duration_s = dur; clips_[i].frame_count = frames;
        };
        set(0, "idle",   0, 2.0f, 60);
        set(1, "walk",   1, 0.8f, 24);
        set(2, "attack", 2, 0.5f, 15);
    }

    static bool ParseInt(const char* p, const char* key, int& out) {
        const char* f = strstr(p, key);
        if (!f) return false;
        f += strlen(key);
        while (*f && (*f == '"' || *f == ':' || *f == ' ')) f++;
        if (!*f) return false;
        out = (int)strtol(f, nullptr, 10);
        return true;
    }
    static bool ParseFloat(const char* p, const char* key, float& out) {
        const char* f = strstr(p, key);
        if (!f) return false;
        f += strlen(key);
        while (*f && (*f == '"' || *f == ':' || *f == ' ')) f++;
        if (!*f) return false;
        out = (float)strtod(f, nullptr);
        return true;
    }
    static bool ParseStr(const char* p, const char* key, char* out, int max) {
        const char* f = strstr(p, key);
        if (!f) return false;
        f += strlen(key);
        const char* q = strchr(f, '"');
        if (!q) return false;
        q++;
        int i = 0;
        while (*q && *q != '"' && i < max - 1) out[i++] = *q++;
        out[i] = '\0';
        return true;
    }
};

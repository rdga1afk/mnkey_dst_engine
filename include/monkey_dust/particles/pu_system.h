// ParticleUniverse system: typed reader over md::pu::Node and the CPU step (emit, affect, move, expire) for one technique.
// Step order and formulas follow the ParticleUniverse sources (MIT, Henry van Merode; test oracle only, engine/THIRD_PARTY.md); written new.
// Covered now: SphereSurface emitter; Colour, Scale, TextureRotator, LinearForce affectors; fixed / dyn_random / dyn_curved_linear attributes.
// Anything else in a script makes LoadSystem fail with the name of the unsupported part (no silent skipping).
#pragma once
#include <cstdint>
#include <string>
#include "monkey_dust/particles/pu_script.h"

namespace md::pu {

constexpr int kMaxCurve = 16, kMaxColours = 16, kMaxEmitters = 8, kMaxAffectors = 12, kMaxParticles = 4096;

// Random source of the step: Ogre Math::UnitRandom is rand() / RAND_MAX, so a test plugs the oracle's generator in here.
struct Rng {
    float (*unit)(void* ctx) = nullptr;
    void* ctx = nullptr;
    float Unit() const { return unit(ctx); }
    float Range(float lo, float hi) const { return (hi - lo) * Unit() + lo; }
};
// The default generator: the MSVC rand() sequence (the original is an MSVC build); `state` is the seed and is advanced.
Rng MsvcRng(uint32_t* state);

struct Dyn {
    enum Type : uint8_t { Fixed, Random, CurvedLinear } type = Fixed;
    float value = 0, lo = 0, hi = 0;
    int n = 0;
    float cx[kMaxCurve] = {}, cy[kMaxCurve] = {};
    float Eval(float x, const Rng& r) const;
};

struct Vec3 { float x = 0, y = 0, z = 0; };
struct Colour { float r = 1, g = 1, b = 1, a = 1; };

struct EmitterDef {
    char name[48] = {};
    Dyn rate, ttl, velocity, angle, all_dim;
    bool has_all_dim = false;
    Colour col_lo{0, 0, 0, 1}, col_hi{1, 1, 1, 1}, col{1, 1, 1, 1};
    bool col_range = false;
    float radius = 1;
    Vec3 dir{0, 1, 0};
    float rate_remainder = 0;   // run state
};

struct AffectorDef {
    enum Kind : uint8_t { ColourAff, ScaleAff, TexRot, LinearForce } kind = ColourAff;
    char name[48] = {};
    // Colour
    int n_col = 0;
    float col_t[kMaxColours] = {};
    Colour col_c[kMaxColours];
    bool col_multiply = false;
    // Scale (xyz only; separate x / y / z are rejected by the reader)
    Dyn scale;
    bool scale_since_start = false;
    // TextureRotator
    Dyn rotation, rotation_speed;
    bool own_speed = true;
    // LinearForce
    Vec3 force;
};

struct TechniqueDef {
    char name[48] = {};
    int quota = 0;
    float default_w = 1, default_h = 1, default_d = 1;
    int lod_index = 0;
    int n_emitters = 0, n_affectors = 0;
    EmitterDef emitters[kMaxEmitters];
    AffectorDef affectors[kMaxAffectors];
    char material[64] = {};
};

struct SystemDef {
    char name[64] = {};
    int n_techniques = 0;
    static constexpr int kMaxTechniques = 4;
    TechniqueDef techniques[kMaxTechniques];
};

// Typed reader of one `system` node. false + `err` names the first unsupported keyword or bad value.
bool LoadSystem(const Node& system_node, SystemDef& out, std::string& err);

struct Particle {
    Vec3 pos, dir, origin;   // origin: the position at birth, before the partial-frame motion
    float ttl = 0, total_ttl = 0, fraction = 0;
    float w = 0, h = 0, d = 0;
    float zrot = 0, zrot_speed = 0;
    Colour colour, original_colour;
    bool emitted_now = false;
    uint32_t serial = 0;
    int emitter = 0;
};

class Instance {
public:
    // `tech`: index into def.techniques. The def is copied (run state lives in the copy).
    void Init(const SystemDef& def, int tech, const Rng& rng);
    void Update(float dt);
    int Count() const { return n_; }
    const Particle& Get(int i) const { return p_[i]; }
    float Time() const { return time_; }
private:
    SystemDef def_;
    int tech_ = 0;
    Rng rng_;
    float time_ = 0;
    uint32_t serial_ = 0;
    int n_ = 0;
    Particle p_[kMaxParticles];
};

}  // namespace md::pu

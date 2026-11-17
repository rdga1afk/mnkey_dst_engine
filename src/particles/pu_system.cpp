// ParticleUniverse system reader and CPU step. Header: monkey_dust/particles/pu_system.h.
#include "monkey_dust/particles/pu_system.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace md::pu {

namespace {

constexpr float kPi = 3.14159265358979f, kTwoPi = 2.0f * kPi;

float MsvcUnit(void* ctx) {
    uint32_t& s = *static_cast<uint32_t*>(ctx);
    s = s * 214013u + 2531011u;
    return float((s >> 16) & 0x7fff) / 32767.0f;
}

Vec3 operator+(Vec3 a, Vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
Vec3 operator*(Vec3 a, float k) { return {a.x * k, a.y * k, a.z * k}; }
Vec3 Cross(Vec3 a, Vec3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
Vec3 Normalised(Vec3 v) {
    float l = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
    return l > 1e-8f ? v * (1.0f / l) : v;
}

// Rotation of `v` by `angle` around `axis` (quaternion form, the axis is used as given).
Vec3 Rotate(Vec3 v, float angle, Vec3 axis) {
    float h = 0.5f * angle, s = std::sin(h), w = std::cos(h);
    Vec3 q{s * axis.x, s * axis.y, s * axis.z};
    Vec3 uv = Cross(q, v), uuv = Cross(q, uv);
    return v + uv * (2.0f * w) + uuv * 2.0f;
}

// Ogre Vector3::perpendicular and randomDeviant with the zero up vector (the emitters here never set one).
Vec3 Perpendicular(Vec3 v) {
    Vec3 p = std::fabs(v.x) > std::fabs(v.z) ? Vec3{-v.y, v.x, 0.0f} : Vec3{0.0f, -v.z, v.y};
    return Normalised(p);
}
Vec3 RandomDeviant(Vec3 v, float angle, const Rng& r) {
    Vec3 up = Rotate(Perpendicular(v), r.Unit() * kTwoPi, v);
    return Rotate(v, angle, up);
}

bool ToFloat(const std::string& s, float& out) {
    char* e = nullptr;
    out = std::strtof(s.c_str(), &e);
    return e != s.c_str() && *e == 0;
}

bool Floats(const Node& n, size_t first, int count, float* out, std::string& err) {
    if (n.args.size() < first + size_t(count)) { err = n.key + " line " + std::to_string(n.line) + ": expected " + std::to_string(count) + " numbers"; return false; }
    for (int i = 0; i < count; ++i)
        if (!ToFloat(n.args[first + size_t(i)], out[i])) { err = n.key + " line " + std::to_string(n.line) + ": bad number '" + n.args[first + size_t(i)] + "'"; return false; }
    return true;
}

bool ReadDyn(const Node& n, Dyn& d, std::string& err) {
    if (n.args.empty()) { err = n.key + " line " + std::to_string(n.line) + ": no value"; return false; }
    const std::string& a = n.args[0];
    if (a == "dyn_random") {
        d.type = Dyn::Random;
        for (const Node& c : n.children) {
            if (c.key == "min" && Floats(c, 0, 1, &d.lo, err)) continue;
            if (c.key == "max" && Floats(c, 0, 1, &d.hi, err)) continue;
            if (err.empty()) err = "dyn_random line " + std::to_string(c.line) + ": unsupported '" + c.key + "'";
            return false;
        }
        return true;
    }
    if (a == "dyn_curved_linear") {
        d.type = Dyn::CurvedLinear;
        for (const Node& c : n.children) {
            float xy[2];
            if (c.key != "control_point") { err = "dyn_curved_linear line " + std::to_string(c.line) + ": unsupported '" + c.key + "'"; return false; }
            if (!Floats(c, 0, 2, xy, err)) return false;
            if (d.n >= kMaxCurve) { err = "dyn_curved_linear line " + std::to_string(c.line) + ": more than " + std::to_string(kMaxCurve) + " control points"; return false; }
            d.cx[d.n] = xy[0]; d.cy[d.n] = xy[1]; ++d.n;
        }
        if (d.n == 0) { err = "dyn_curved_linear line " + std::to_string(n.line) + ": no control points"; return false; }
        for (int i = 1; i < d.n; ++i)   // insertion sort by x (ParticleUniverse sorts the control points once)
            for (int j = i; j > 0 && d.cx[j] < d.cx[j - 1]; --j) { std::swap(d.cx[j], d.cx[j - 1]); std::swap(d.cy[j], d.cy[j - 1]); }
        return true;
    }
    if (a.compare(0, 4, "dyn_") == 0) { err = n.key + " line " + std::to_string(n.line) + ": unsupported attribute type " + a; return false; }
    d.type = Dyn::Fixed;
    return Floats(n, 0, 1, &d.value, err);
}

void CopyName(char* dst, size_t cap, const std::string& s) { std::snprintf(dst, cap, "%s", s.c_str()); }

bool ReadEmitter(const Node& n, EmitterDef& e, std::string& err) {
    if (n.args.empty() || n.args[0] != "SphereSurface") { err = "emitter line " + std::to_string(n.line) + ": unsupported type " + (n.args.empty() ? std::string("?") : n.args[0]); return false; }
    if (n.args.size() > 1) CopyName(e.name, sizeof e.name, n.args[1]);
    e.rate.value = 10; e.ttl.value = 3; e.velocity.value = 100; e.angle.value = 20;
    for (const Node& c : n.children) {
        float f[4];
        if (c.key == "emission_rate") { if (!ReadDyn(c, e.rate, err)) return false; }
        else if (c.key == "angle") { if (!ReadDyn(c, e.angle, err)) return false; }
        else if (c.key == "time_to_live") { if (!ReadDyn(c, e.ttl, err)) return false; }
        else if (c.key == "velocity") { if (!ReadDyn(c, e.velocity, err)) return false; }
        else if (c.key == "all_particle_dimensions") { if (!ReadDyn(c, e.all_dim, err)) return false; e.has_all_dim = true; }
        else if (c.key == "start_colour_range") { if (!Floats(c, 0, 4, f, err)) return false; e.col_lo = {f[0], f[1], f[2], f[3]}; e.col_range = true; }
        else if (c.key == "end_colour_range") { if (!Floats(c, 0, 4, f, err)) return false; e.col_hi = {f[0], f[1], f[2], f[3]}; e.col_range = true; }
        else if (c.key == "colour") { if (!Floats(c, 0, 4, f, err)) return false; e.col = {f[0], f[1], f[2], f[3]}; }
        else if (c.key == "radius") { if (!Floats(c, 0, 1, &e.radius, err)) return false; }
        else { err = "SphereSurface emitter line " + std::to_string(c.line) + ": unsupported property '" + c.key + "'"; return false; }
    }
    return true;
}

bool ReadAffector(const Node& n, AffectorDef& a, std::string& err) {
    if (n.args.empty()) { err = "affector line " + std::to_string(n.line) + ": no type"; return false; }
    const std::string& t = n.args[0];
    if (n.args.size() > 1) CopyName(a.name, sizeof a.name, n.args[1]);
    if (t == "Colour") {
        a.kind = AffectorDef::ColourAff;
        for (const Node& c : n.children) {
            float f[5];
            if (c.key == "time_colour") {
                if (!Floats(c, 0, 5, f, err)) return false;
                int i = 0;
                while (i < a.n_col && a.col_t[i] != f[0]) ++i;   // a repeated time replaces (std::map semantics)
                if (i == a.n_col) { if (a.n_col >= kMaxColours) { err = "Colour affector line " + std::to_string(c.line) + ": too many time_colour"; return false; } ++a.n_col; }
                a.col_t[i] = f[0]; a.col_c[i] = {f[1], f[2], f[3], f[4]};
            } else if (c.key == "colour_operation") {
                if (c.args.empty() || (c.args[0] != "multiply" && c.args[0] != "set")) { err = "Colour affector line " + std::to_string(c.line) + ": bad colour_operation"; return false; }
                a.col_multiply = c.args[0] == "multiply";
            } else { err = "Colour affector line " + std::to_string(c.line) + ": unsupported property '" + c.key + "'"; return false; }
        }
        for (int i = 1; i < a.n_col; ++i)
            for (int j = i; j > 0 && a.col_t[j] < a.col_t[j - 1]; --j) { std::swap(a.col_t[j], a.col_t[j - 1]); std::swap(a.col_c[j], a.col_c[j - 1]); }
        return true;
    }
    if (t == "Scale") {
        a.kind = AffectorDef::ScaleAff;
        bool xyz = false;
        for (const Node& c : n.children) {
            if (c.key == "xyz_scale") { if (!ReadDyn(c, a.scale, err)) return false; xyz = true; }
            else if (c.key == "since_system_start") { a.scale_since_start = !c.args.empty() && c.args[0] == "true"; }
            else { err = "Scale affector line " + std::to_string(c.line) + ": unsupported property '" + c.key + "'"; return false; }
        }
        if (!xyz) { err = "Scale affector line " + std::to_string(n.line) + ": only xyz_scale is supported"; return false; }
        return true;
    }
    if (t == "TextureRotator") {
        a.kind = AffectorDef::TexRot;
        a.rotation.value = 0; a.rotation_speed.value = 0;
        for (const Node& c : n.children) {
            if (c.key == "rotation") { if (!ReadDyn(c, a.rotation, err)) return false; }
            else if (c.key == "rotation_speed") { if (!ReadDyn(c, a.rotation_speed, err)) return false; }
            else if (c.key == "use_own_rotation") { a.own_speed = c.args.empty() || c.args[0] == "true"; }
            else { err = "TextureRotator affector line " + std::to_string(c.line) + ": unsupported property '" + c.key + "'"; return false; }
        }
        return true;
    }
    if (t == "LinearForce") {
        a.kind = AffectorDef::LinearForce;
        for (const Node& c : n.children) {
            float f[3];
            if (c.key == "force_vector") { if (!Floats(c, 0, 3, f, err)) return false; a.force = {f[0], f[1], f[2]}; }
            else { err = "LinearForce affector line " + std::to_string(c.line) + ": unsupported property '" + c.key + "'"; return false; }
        }
        return true;
    }
    err = "affector line " + std::to_string(n.line) + ": unsupported type " + t;
    return false;
}

bool ReadTechnique(const Node& n, TechniqueDef& t, std::string& err) {
    if (!n.args.empty()) CopyName(t.name, sizeof t.name, n.args[0]);
    for (const Node& c : n.children) {
        float f = 0;
        if (c.key == "visual_particle_quota") { if (!Floats(c, 0, 1, &f, err)) return false; t.quota = int(f); }
        else if (c.key == "emitted_emitter_quota" || c.key == "emitted_technique_quota" || c.key == "emitted_affector_quota") {}
        else if (c.key == "material") { if (!c.args.empty()) CopyName(t.material, sizeof t.material, c.args[0]); }
        else if (c.key == "lod_index") { if (!Floats(c, 0, 1, &f, err)) return false; t.lod_index = int(f); }
        else if (c.key == "default_particle_width") { if (!Floats(c, 0, 1, &t.default_w, err)) return false; }
        else if (c.key == "default_particle_height") { if (!Floats(c, 0, 1, &t.default_h, err)) return false; }
        else if (c.key == "default_particle_depth") { if (!Floats(c, 0, 1, &t.default_d, err)) return false; }
        else if (c.key == "renderer") {
            if (c.args.empty() || c.args[0] != "Billboard") { err = "renderer line " + std::to_string(c.line) + ": unsupported type"; return false; }
        }
        else if (c.key == "emitter") {
            if (t.n_emitters >= kMaxEmitters) { err = "technique " + std::string(t.name) + ": more than " + std::to_string(kMaxEmitters) + " emitters"; return false; }
            if (!ReadEmitter(c, t.emitters[t.n_emitters++], err)) return false;
        }
        else if (c.key == "affector") {
            if (t.n_affectors >= kMaxAffectors) { err = "technique " + std::string(t.name) + ": more than " + std::to_string(kMaxAffectors) + " affectors"; return false; }
            if (!ReadAffector(c, t.affectors[t.n_affectors++], err)) return false;
        }
        else { err = "technique " + std::string(t.name) + " line " + std::to_string(c.line) + ": unsupported property '" + c.key + "'"; return false; }
    }
    if (t.quota <= 0 || t.quota > kMaxParticles) { err = "technique " + std::string(t.name) + ": visual_particle_quota " + std::to_string(t.quota) + " outside 1.." + std::to_string(kMaxParticles); return false; }
    return true;
}

}  // namespace

Rng MsvcRng(uint32_t* state) { Rng r; r.unit = MsvcUnit; r.ctx = state; return r; }

float Dyn::Eval(float x, const Rng& r) const {
    switch (type) {
    case Fixed: return value;
    case Random: return r.Range(lo, hi);
    case CurvedLinear: {
        if (n == 0) return 0;
        int i = n - 1;   // the nearest control point: the last one whose x is <= the argument
        for (int k = 0; k < n; ++k)
            if (x < cx[k]) { i = k == 0 ? 0 : k - 1; break; }
        if (i + 1 >= n) return cy[i];
        return cy[i] + (cy[i + 1] - cy[i]) * (x - cx[i]) / (cx[i + 1] - cx[i]);
    }
    }
    return 0;
}

bool LoadSystem(const Node& s, SystemDef& out, std::string& err) {
    out = SystemDef{};
    if (s.key != "system") { err = "not a system node"; return false; }
    if (!s.args.empty()) CopyName(out.name, sizeof out.name, s.args[0]);
    for (const Node& c : s.children) {
        if (c.key == "technique") {
            if (out.n_techniques >= SystemDef::kMaxTechniques) { err = "system " + std::string(out.name) + ": more than " + std::to_string(SystemDef::kMaxTechniques) + " techniques"; return false; }
            if (!ReadTechnique(c, out.techniques[out.n_techniques++], err)) return false;
        }
        else if (c.key == "smooth_lod" || c.key == "category" || c.key == "lod_distances") {}
        else { err = "system " + std::string(out.name) + " line " + std::to_string(c.line) + ": unsupported property '" + c.key + "'"; return false; }
    }
    return true;
}

void Instance::Init(const SystemDef& def, int tech, const Rng& rng) {
    def_ = def; tech_ = tech; rng_ = rng; time_ = 0; serial_ = 0; n_ = 0;
}

void Instance::Update(float dt) {
    TechniqueDef& t = def_.techniques[tech_];
    time_ += dt;   // the system clock advances before the technique runs

    // emit: every emitter in script order
    for (int ei = 0; ei < t.n_emitters; ++ei) {
        EmitterDef& e = t.emitters[ei];
        float rate = e.rate.Eval(time_, rng_);
        e.rate_remainder += rate * dt;
        unsigned short requested = (unsigned short)e.rate_remainder;
        e.rate_remainder -= requested;
        float time_inc = dt / float(requested), time_point = 0;
        for (unsigned j = 0; j < requested; ++j) {
            if (n_ >= t.quota) break;
            Particle& p = p_[n_++];
            p = Particle{};
            p.serial = serial_++; p.emitter = ei; p.emitted_now = true;
            Vec3 v;   // the source builds Vector3(r, r, r) from three calls: the order is unspecified in C++; the oracle (GCC) draws z, y, x
            v.z = rng_.Range(-1, 1); v.y = rng_.Range(-1, 1); v.x = rng_.Range(-1, 1);
            v = Normalised(v);
            p.pos = v * e.radius;
            p.origin = p.pos;
            float ang = e.angle.Eval(time_, rng_) * (kPi / 180.0f);
            if (e.angle.type == Dyn::Fixed) ang = rng_.Unit() * ang;   // a fixed angle is a maximum, not the value
            p.dir = ang != 0.0f ? RandomDeviant(e.dir, ang, rng_) : e.dir;
            p.dir = p.dir * e.velocity.Eval(time_, rng_);
            if (e.col_range) {
                p.colour.r = rng_.Range(e.col_lo.r, e.col_hi.r);
                p.colour.g = rng_.Range(e.col_lo.g, e.col_hi.g);
                p.colour.b = rng_.Range(e.col_lo.b, e.col_hi.b);
                p.colour.a = rng_.Range(e.col_lo.a, e.col_hi.a);
            } else p.colour = e.col;
            p.original_colour = p.colour;
            p.total_ttl = e.ttl.Eval(time_, rng_); p.ttl = p.total_ttl;
            if (e.has_all_dim) { float x = e.all_dim.Eval(time_, rng_); p.w = p.h = p.d = x; }
            else { p.w = t.default_w; p.h = t.default_h; p.d = t.default_d; }
            for (int ai = 0; ai < t.n_affectors; ++ai) {
                const AffectorDef& a = t.affectors[ai];
                if (a.kind != AffectorDef::TexRot) continue;
                p.zrot = a.rotation.Eval(time_, rng_);
                p.zrot_speed = a.rotation_speed.Eval(0.0f, rng_);
            }
            p.pos = p.pos + p.dir * time_point;   // partial frame motion
            time_point += time_inc;
        }
    }

    // process: expire, affect, move
    // The pool walk of ParticleUniverse: an expired particle is unlinked and the walk steps back one place, unless it was the first
    // of the list, then the new first particle is stepped over and gets no update this frame (it keeps its emitted flag).
    int i = 0;
    while (i < n_) {
        Particle p = p_[i];
        if (p.ttl < dt) {   // expired this frame
            std::memmove(&p_[i], &p_[i + 1], size_t(n_ - i - 1) * sizeof(Particle));
            --n_;
            if (i != 0 && i != n_) --i;
            if (i == n_) break;
            ++i;
            continue;
        }
        p.fraction = (p.total_ttl - p.ttl) / p.total_ttl;
        for (int ai = 0; ai < t.n_affectors; ++ai) {
            const AffectorDef& a = t.affectors[ai];
            switch (a.kind) {
            case AffectorDef::ColourAff: {
                if (a.n_col == 0) break;
                int k = a.n_col - 1;
                for (int m = 0; m < a.n_col; ++m)
                    if (p.fraction < a.col_t[m]) { k = m == 0 ? 0 : m - 1; break; }
                Colour c = a.col_c[k];
                if (k + 1 < a.n_col) {
                    float u = (p.fraction - a.col_t[k]) / (a.col_t[k + 1] - a.col_t[k]);
                    const Colour& d = a.col_c[k + 1];
                    c = {c.r + (d.r - c.r) * u, c.g + (d.g - c.g) * u, c.b + (d.b - c.b) * u, c.a + (d.a - c.a) * u};
                }
                if (a.col_multiply) c = {c.r * p.original_colour.r, c.g * p.original_colour.g, c.b * p.original_colour.b, c.a * p.original_colour.a};
                p.colour = c;
                break;
            }
            case AffectorDef::ScaleAff: {
                float ds = a.scale.Eval(a.scale_since_start ? time_ : p.fraction, rng_) * dt;
                if (p.w + ds > 0) p.w += ds;   // a dimension that would reach 0 keeps its value (setOwnDimensions ignores 0)
                if (p.h + ds > 0) p.h += ds;
                if (p.d + ds > 0) p.d += ds;
                break;
            }
            case AffectorDef::TexRot: {
                float speed = a.own_speed ? p.zrot_speed * dt : a.rotation_speed.Eval(p.fraction, rng_) * dt;
                p.zrot += speed;
                if (p.zrot > kTwoPi) p.zrot -= kTwoPi;
                break;
            }
            case AffectorDef::LinearForce:
                p.dir = p.dir + a.force * dt;
                break;
            }
        }
        if (!p.emitted_now) p.pos = p.pos + p.dir * dt;
        p.emitted_now = false;
        p.ttl -= dt;
        p_[i++] = p;
    }
}

}  // namespace md::pu

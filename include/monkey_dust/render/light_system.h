#pragma once
#include <monkey_dust/platform/math_types.h>
#include <monkey_dust/render/md_shader.h>
#include <monkey_dust/render/md_texture.h>
#include <cmath>
#include <cstdint>
#include <cstring>

// ─────────────────────────────────────────────────────────
// LightSystem — directional sun + ambient IBL.
//
// BRDF LUT (64×64, RGBA8): precomputed split-sum for specular.
//   X axis = NdotV, Y axis = roughness.
//   R = F0 scale, G = F0 bias (standard Karis split-sum).
//
// Apply(shader): sets sunDir, sunColor, ambientColor uniforms.
// Bind(): binds brdfLUT to texture unit 8 (slot > PBR atlas range).
// ─────────────────────────────────────────────────────────

static constexpr int BRDF_LUT_SIZE = 64;

class LightSystem {
public:
    static LightSystem& Get() {
        static LightSystem inst;
        return inst;
    }

    Vec3 sun_dir      = { 0.577f, -0.577f, 0.577f }; // normalized, sun→surface
    Vec3 sun_color    = { 1.00f, 0.95f, 0.80f };
    Vec3 ambient_color = { 0.15f, 0.18f, 0.22f };

    // 2026-09-20 (F3 Settings tab, docs/RESOLVE_OPT.md session): manual
    // time-of-day override for dev/testing -- when enabled, UpdateGameState
    // (logic_tick_orchestration.cpp) uses time_override_hours instead of the
    // real now_s/60 elapsed-time clock. Lives here (not a game-side global)
    // so both game/src (consumer) and tools/editor (the F3 UI control) reach
    // it through the same LightSystem::Get() singleton both already use.
    bool  time_override_enabled = false;
    float time_override_hours   = 12.0f;

    MdTexture brdf_lut;

    void Init() {
        static uint8_t pixels[BRDF_LUT_SIZE * BRDF_LUT_SIZE * 4];
        GenerateBRDFLUT(pixels, BRDF_LUT_SIZE);
        brdf_lut = MdLoadTextureFromMemory(pixels, BRDF_LUT_SIZE, BRDF_LUT_SIZE);
    }

    // W-1: Day/night cycle — animate sun_dir, sun_color, ambient_color from game time.
    // game_time_hours ∈ [0, 24). Roadmap: morning=warm yellow, noon=white, night=dark blue.
    //   6h  = sunrise  (sun on E horizon)
    //  12h  = noon     (sun at peak)
    //  18h  = sunset   (sun on W horizon)
    //   0h  = midnight (sun fully below horizon)
    void Tick(float game_time_hours) noexcept {
        // Sun elevation: sin curve peaking at noon (12h), zero at 6h and 18h
        const float pi    = 3.14159265f;
        float day_frac    = game_time_hours / 24.f;             // [0,1)
        float elev_angle  = (day_frac - 0.25f) * 2.f * pi;     // 6h=0, 12h=π/2, 18h=π
        float elevation   = sinf(elev_angle);                   // -1..+1 (+ = above horizon)
        float azimuth     = cosf(elev_angle);                   // east→west component

        // sun_dir = direction from sun to surface (light direction)
        sun_dir.x = azimuth * 0.6f;
        sun_dir.y = -elevation;   // negative because y points up, sun_dir points down
        sun_dir.z = 0.5f;         // slight south bias (N hemisphere)
        // normalize
        float len = sqrtf(sun_dir.x*sun_dir.x + sun_dir.y*sun_dir.y + sun_dir.z*sun_dir.z);
        if (len > 0.001f) { sun_dir.x/=len; sun_dir.y/=len; sun_dir.z/=len; }

        // t_day: 0=night, 1=full day (smooth step from elevation)
        float t_day = elevation < 0.f ? 0.f : elevation;
        if (t_day > 1.f) t_day = 1.f;

        // dawn/dusk blend: warm orange tint near horizon (elevation ∈ [-0.2, 0.2])
        float t_horiz = 1.f - fabsf(elevation) / 0.35f;
        if (t_horiz < 0.f) t_horiz = 0.f;
        if (t_horiz > 1.f) t_horiz = 1.f;
        t_horiz *= t_day > 0.f ? 1.f : 0.f; // only during daylight side

        // --- Sun color ---
        // night=black, dawn/dusk=warm orange, day=white
        const Vec3 c_day   = { 1.00f, 0.97f, 0.88f };   // warm white noon
        const Vec3 c_horiz = { 1.00f, 0.62f, 0.20f };   // orange horizon
        const Vec3 c_night = { 0.00f, 0.00f, 0.00f };   // no direct sun at night
        Vec3 sc = lerpV(c_night, c_day, t_day);
        sc = lerpV(sc, c_horiz, t_horiz * 0.65f);

        // 2026-09-20 (owner-caught, docs/RESOLVE_OPT.md session): night had
        // NO celestial light source at all -- `moon` didn't exist anywhere
        // in the codebase (grep confirmed zero hits before this), so
        // c_night=black left terrain lit by ambient_color's near-black
        // {0.02,0.02,0.06} "stars" floor alone. Moon direction/elevation is
        // the sun's elev_angle phase-shifted by π (12h) -- the moon is
        // "opposite" the sun in this simple model, so it's above the
        // horizon exactly when the sun (t_day==0) is below it, and vice
        // versa (moon_elevation == -elevation, verified algebraically:
        // sin(elev_angle+π) == -sin(elev_angle)). Reuses sun_dir/sun_color
        // (not new fields) -- at night these represent the moon instead of
        // the sun, so the whole existing pipeline (terrain's uf.sun_dir_str,
        // NPC's sunDir/sunColor, AmbientProbeSystem's SetSkyLight) picks up
        // moonlight for free, no shader/UBO changes needed.
        float moon_elevation = -elevation;
        float t_moon = moon_elevation < 0.f ? 0.f : moon_elevation;
        if (t_moon > 1.f) t_moon = 1.f;
        // Cool, dim bluish-white -- real moonlight is reflected sunlight,
        // much weaker and cooler than direct sun (no dawn/dusk warm tint;
        // a full moon doesn't meaningfully change color near the horizon
        // the way the sun's atmospheric scattering does).
        const Vec3 c_moon = { 0.32f, 0.36f, 0.52f };
        Vec3 mc = vec3_scale(c_moon, t_moon);
        if (t_day <= 0.f) {
            // Real SkyX BasicController::update(): mMoonDirection = -mSunDirection
            // (full 3-component negation, moon is antipodal to the sun).
            sun_dir.x = -sun_dir.x;
            sun_dir.y = -sun_dir.y;
            sun_dir.z = -sun_dir.z;
            sc = mc;
        }
        sun_color = sc;

        // --- Ambient color ---
        // night=very dark blue, dawn=purple, day=soft blue, dusk=orange-purple
        const Vec3 a_day   = { 0.18f, 0.22f, 0.30f };
        const Vec3 a_night = { 0.02f, 0.02f, 0.06f };   // near-black, slight blue (stars)
        const Vec3 a_horiz = { 0.22f, 0.14f, 0.20f };   // purple horizon glow
        Vec3 ac = lerpV(a_night, a_day, t_day);
        ac = lerpV(ac, a_horiz, t_horiz * 0.5f);
        ambient_color = ac;
    }

    // VBfA-R3: compute 6-sample directional light probe for NPC ambient.
    // probe_out: float[24] — 6 groups of (R, G, B, pad), one per ±XYZ direction.
    // Order: [0]=+X [1]=-X [2]=+Y [3]=-Y [4]=+Z [5]=-Z
    // Formula: probe[i] = ambient + max(0, dot(dir_i, toSun)) * sun_color * kSun
    void ComputeProbe(float probe_out[24]) const {
        static const float kDirs[6][3] = {
            { 1, 0, 0}, {-1, 0, 0},
            { 0, 1, 0}, { 0,-1, 0},
            { 0, 0, 1}, { 0, 0,-1}
        };
        // sun_dir = sun→surface direction; toSun = -sun_dir (surface→sun)
        const float ts[3] = { -sun_dir.x, -sun_dir.y, -sun_dir.z };
        for (int i = 0; i < 6; i++) {
            float ndotl = kDirs[i][0]*ts[0] + kDirs[i][1]*ts[1] + kDirs[i][2]*ts[2];
            if (ndotl < 0.f) ndotl = 0.f;
            probe_out[i*4+0] = ambient_color.x + sun_color.x * ndotl * 0.45f;
            probe_out[i*4+1] = ambient_color.y + sun_color.y * ndotl * 0.45f;
            probe_out[i*4+2] = ambient_color.z + sun_color.z * ndotl * 0.45f;
            probe_out[i*4+3] = 0.f;
        }
    }

    // Sets sunDir, sunColor, ambientColor uniforms on any shader by name.
    // sunDir is "to sun" in fragment shader convention (light dir reversed).
    void Apply(MdShader shader) const {
        int locDir = MdGetLoc(shader, "sunDir");
        int locSun = MdGetLoc(shader, "sunColor");
        int locAmb = MdGetLoc(shader, "ambientColor");
        float toSun[3] = { -sun_dir.x, -sun_dir.y, -sun_dir.z };
        MdSetVec3(locDir, toSun);
        MdSetVec3(locSun, &sun_color.x);
        MdSetVec3(locAmb, &ambient_color.x);
    }

    void Shutdown() {
        MdUnloadTexture(brdf_lut);
    }

private:
    LightSystem() = default;
    static Vec3 lerpV(Vec3 a, Vec3 b, float t) noexcept {
        return { a.x + (b.x-a.x)*t, a.y + (b.y-a.y)*t, a.z + (b.z-a.z)*t };
    }

    // Split-sum BRDF LUT via GGX importance sampling (Hammersley, 128 samples).
    static void GenerateBRDFLUT(uint8_t* out, int size) {
        for (int y = 0; y < size; ++y) {
            for (int x = 0; x < size; ++x) {
                float NdotV    = (x + 0.5f) / (float)size;
                float roughness = (y + 0.5f) / (float)size;
                float a  = roughness * roughness;
                float Vx = sqrtf(1.0f - NdotV * NdotV);
                float Vz = NdotV;

                float scale = 0.0f, bias = 0.0f;
                const int N = 128;
                for (int i = 0; i < N; ++i) {
                    // Hammersley low-discrepancy sequence
                    float phi  = 6.28318530f * (float)i / (float)N;
                    uint32_t bits = (uint32_t)i;
                    bits = (bits << 16u) | (bits >> 16u);
                    bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
                    bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
                    bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
                    bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
                    float xi = (float)bits * 2.3283064365e-10f;

                    // GGX importance sample half-vector
                    float cosT = sqrtf((1.0f - xi) / (1.0f + (a*a - 1.0f) * xi + 1e-7f));
                    float sinT = sqrtf(fmaxf(0.0f, 1.0f - cosT * cosT));
                    float Hx   = cosf(phi) * sinT;
                    float Hz   = cosT;

                    float VdotH = Vx * Hx + Vz * Hz;
                    float NdotL = fmaxf(2.0f * VdotH * Hz - Vz, 0.0f);
                    float NdotH = fmaxf(Hz, 0.0f);

                    if (NdotL > 0.0f) {
                        // Smith-Schlick geometry
                        float k    = a * 0.5f;
                        float G1v  = NdotV / (NdotV * (1.0f - k) + k);
                        float G1l  = NdotL / (NdotL * (1.0f - k) + k);
                        float G_vis = G1v * G1l * VdotH / fmaxf(NdotH * NdotV, 1e-5f);
                        float Fc   = powf(1.0f - VdotH, 5.0f);
                        scale += (1.0f - Fc) * G_vis;
                        bias  += Fc           * G_vis;
                    }
                }
                scale /= (float)N;
                bias  /= (float)N;

                int idx = (y * size + x) * 4;
                out[idx + 0] = (uint8_t)(fminf(fmaxf(scale, 0.0f), 1.0f) * 255.0f + 0.5f);
                out[idx + 1] = (uint8_t)(fminf(fmaxf(bias,  0.0f), 1.0f) * 255.0f + 0.5f);
                out[idx + 2] = 0;
                out[idx + 3] = 255;
            }
        }
    }
};

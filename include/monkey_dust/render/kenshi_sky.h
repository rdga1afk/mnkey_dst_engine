#pragma once
// Kenshi's sky light on the CPU (#140 L1): the sun's path (exe SkyXController::update), the sun colour and strength (F040805), the sky's atmospheric colour
// (single scattering after O'Neil, "Accurate Atmospheric Scattering", GPU Gems 2 ch. 16, with Kenshi's Options as the exe sets them; the HDR night glow follows the SkyX atmosphere colour function),
// horizonClouds, zenithLight, pFogParams and MIN_LUMINANCE. The scattering and the sun path are rewritten from the published method and our own reads (no SkyX code); the remaining parts are a port of Project-Okran src/render/sky.rs (MIT).
// Constants of the GLOBAL CONSTANTS record 110-gamedata.quack (sunrise 5, sunset 23, latitude 54, exposure min 0.8 / max 1.2, night darkness 0.35: read from the install, same values).
#include <algorithm>
#include <cmath>

namespace md {

inline float g_view_distance_m = 5000.f;
inline float ViewFarClipM() { return g_view_distance_m < 4000.f ? g_view_distance_m : 4000.f; }   // camera far clip / HZB / terrain cull: V up to 4000 m; beyond it the depth precision of the ground reconstruction changes the frames (gate frames at V 5000 and far 5000: 14 of 15 views differ)   // the "View Distance" option (settings.cfg, Kenshi default 5000): D = value x 10 units; the camera far clip, the fog and the skydome radius follow it

struct KsVec3 { float x = 0.f, y = 0.f, z = 0.f; };
struct KsVec4 { float x = 0.f, y = 0.f, z = 0.f, w = 0.f; };

struct KenshiAtmosphere {   // SkyX AtmosphereManager::Options as Kenshi sets them (F040804)
    float inner_radius = 9.77501f, outer_radius = 10.2963f, height_position = 0.01f;
    float rayleigh = 0.0022f, mie = 0.000675f, sun_intensity = 30.f;
    KsVec3 wavelength = {0.57f, 0.48f, 0.44f};
    float g = -0.991f, exposure = 1.4f;
    int samples = 4;
};

struct KenshiSkySettings {
    float sunrise = 5.f, sunset = 23.f;   // hours
    float tilt = 54.f * 3.14159265358979f / 180.f;   // the "latitude", radians
    float exposure_min = 0.8f, exposure_max = 1.2f, night_darkness = 0.35f;
    KenshiAtmosphere atmosphere;
    float view_distance = g_view_distance_m;
    KsVec3 clouds = {3.f, -0.8f, 0.f};    // cloud layer DensityMultiplier, DensityOffset, Darkness (F040804)
    KsVec3 sky_mult = {1.f, 1.f, 1.f};    // the weather's sky colour multiplier

    struct MainLight { KsVec3 dir; KsVec3 colour; float w = 0.f; float min_lum = 0.f; };

    // optical-depth fit of GPU Gems 2 ch. 16 (O'Neil, listing "scale"); equals the shader-side fit of the SkyX dome
    static float Scale(float cos_a, float scale_depth) {
        const float x = 1.0f - cos_a;
        return scale_depth * std::exp(x * (0.459f + x * (3.83f + x * (-6.80f + x * 5.25f))) - 0.00287f);
    }
    static float Dot(KsVec3 a, KsVec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
    static KsVec3 Norm(KsVec3 a) { const float l = std::sqrt(Dot(a, a)); return l > 0.f ? KsVec3{a.x / l, a.y / l, a.z / l} : KsVec3{}; }

    // Sky colour toward `direction` (unit, from the ground) with the sun toward `sun` (unit): single scattering after O'Neil, GPU Gems 2 ch. 16,
    // with the Kenshi parameters (the exe's Options) plus the HDR night glow of the SkyX atmosphere colour function.
    KsVec3 ColorAt(KsVec3 dir, KsVec3 sun) const {
        dir = Norm(dir);
        const KenshiAtmosphere& A = atmosphere;
        const float PI = 3.14159265358979f;
        const float thick = A.outer_radius - A.inner_radius, scale = 1.0f / thick, depth_scale = thick * 0.5f, sod = scale / depth_scale;
        const float cam_y = A.inner_radius + thick * A.height_position;
        // the dome point of the direction in atmosphere space and the view ray from the camera to it
        const KsVec3 p = {dir.x, dir.y + A.inner_radius, dir.z};
        KsVec3 ray = {p.x, p.y - cam_y, p.z};
        const float far_len = std::sqrt(Dot(ray, ray));
        ray = {ray.x / far_len, ray.y / far_len, ray.z / far_len};
        const float start_offset = std::exp(sod * (A.inner_radius - cam_y)) * Scale(ray.y, depth_scale);
        const float step_len = far_len / (float)A.samples, scaled = step_len * scale;
        const KsVec3 step = {ray.x * step_len, ray.y * step_len, ray.z * step_len};
        KsVec3 s = {step.x * 0.5f, cam_y + step.y * 0.5f, step.z * 0.5f};
        const float iw[3] = {1.f / std::pow(A.wavelength.x, 4.f), 1.f / std::pow(A.wavelength.y, 4.f), 1.f / std::pow(A.wavelength.z, 4.f)};
        float acc[3] = {0.f, 0.f, 0.f};
        for (int i = 0; i < A.samples; ++i) {
            const float h = std::sqrt(Dot(s, s));
            const float d = std::exp(sod * (A.inner_radius - h));
            const float out = start_offset + d * (Scale(Dot(sun, s) / h, depth_scale) - Scale(Dot(ray, s) / h, depth_scale));
            for (int c = 0; c < 3; ++c) acc[c] += std::exp(-out * (iw[c] * A.rayleigh * 4.f * PI + A.mie * 4.f * PI)) * (d * scaled);
            s = {s.x + step.x, s.y + step.y, s.z + step.z};
        }
        // phase functions, theta = angle between the sun and the direction from the dome point back to the camera
        const KsVec3 back = {-p.x, cam_y - p.y, -p.z};
        const float cs = Dot(sun, back) / std::sqrt(Dot(back, back)), cs2 = cs * cs, g2 = A.g * A.g;
        const float ph_r = 0.75f * (1.0f + 0.5f * cs2);
        const float ph_m = 1.5f * ((1.0f - g2) / (2.0f + g2)) * (1.0f + cs2) / std::pow(1.0f + g2 - 2.0f * A.g * cs, 1.5f);
        KsVec3 col;
        float* o[3] = {&col.x, &col.y, &col.z};
        for (int c = 0; c < 3; ++c) *o[c] = A.exposure * (ph_r * acc[c] * iw[c] * A.rayleigh * A.sun_intensity + ph_m * acc[c] * A.mie * A.sun_intensity);
        // night glow: fades out when the sky is bright, bluer and stronger toward the horizon, weaker with the sun far below it
        const float mx = std::max(col.x, std::max(col.y, col.z));
        const float night = std::min(std::max(1.0f - mx * 10.0f, 0.0f), 1.0f);
        const float k = 2.0f - std::min(std::max(-sun.y, 0.0f), 1.0f) * 0.75f;
        const float hz = std::pow(1.0f - dir.y, 3.0f);
        col.x += night * std::pow(k * 0.05f * hz, 2.2f);
        col.y += night * std::pow(k * 0.05f * hz, 2.2f);
        col.z += night * std::pow(k * 0.10f * hz, 2.2f);
        return col;
    }

    // The sun's unit direction at an hour (0..24): exe SkyXController::update:
    // fraction f of the way through the day (0 at sunrise, 1 at sunset) or through the night (1..2); direction = (cos f*PI, cos(tilt) sin f*PI, sin(tilt) sin f*PI).
    KsVec3 SunDirection(float hour) const {
        const float PI = 3.14159265358979f;
        float f;
        if (hour < sunrise || hour > sunset) {
            const float t = hour < sunrise ? hour + 24.f : hour;
            f = (t - sunset) / (24.f - (sunset - sunrise)) + 1.f;
        } else {
            f = (hour - sunrise) / (sunset - sunrise);
        }
        return {std::cos(f * PI), std::cos(tilt) * std::sin(f * PI), std::sin(tilt) * std::sin(f * PI)};
    }

    // F040805: sunDirection (the sun with y clamped to 0), sunColour rgb, its strength w, MIN_LUMINANCE
    MainLight GetMainLight(float hour) const {
        const KsVec3 real = SunDirection(hour);
        MainLight L;
        if (real.y >= -0.2f) {
            const KsVec3 c = ColorAt({real.x, real.y, real.z + 0.04f}, real);
            const float d = atmosphere.exposure * 4.f;
            L.colour = {c.x / d, c.y / d, c.z / d};
        }
        const double base = 0.5;
        double w = real.y <= 0.f ? (0.0 - base) * (double)real.y * -10.8 + base : (1.0 - base) * (double)real.y + base;
        L.w = (float)std::max(w, 0.0);
        L.dir = real;
        if (L.dir.y < 0.f) { L.dir.y = 0.f; L.dir = Norm(L.dir); }
        const float day = std::min(std::max(real.y * 5.f, 0.f), 1.f);
        L.min_lum = ((1.f - night_darkness) * day + night_darkness) * exposure_min;
        return L;
    }

    // pFogParams: (D, 0.06 D, 0.6 D, weather density = 0), D = View Distance x 10
    KsVec4 FogParams() const { const float d = view_distance * 10.f; return {d, d * 0.06f, std::min(d * 0.6f, d), 0.f}; }

    // horizonClouds (F040805): (saturate(...) x sqrt(exposure), saturate(offset + 0.5))
    KsVec4 HorizonClouds(float hour) const {
        const KsVec3 real = SunDirection(hour);
        const MainLight L = GetMainLight(hour);
        const float n = clouds.x, m = clouds.y, k = clouds.z;
        const float c = (m + 0.2f) * n;
        KsVec3 z = ColorAt({0.f, 0.f, 1.f}, real);
        z = {std::max(z.x * sky_mult.x, 0.001f), std::max(z.y * sky_mult.y, 0.001f), std::max(z.z * sky_mult.z, 0.0015f)};
        const float e = std::sqrt(atmosphere.exposure);
        auto ch = [&](float col, float zen) { return std::min(std::max((col * (1.f - c * 0.1f) + zen * L.colour.y) * (1.f - k), 0.f), 1.f) * e; };
        return {ch(L.colour.x, z.x), ch(L.colour.y, z.y), ch(L.colour.z, z.z), std::min(std::max(m + 0.5f, 0.f), 1.f)};
    }

    // zenithLight (F040805): getColorAt(UNIT_Z) x the weather's sky multiplier, at least the minimum colour, x the sun colour's green
    KsVec3 ZenithLight(float hour) const {
        const KsVec3 real = SunDirection(hour);
        const MainLight L = GetMainLight(hour);
        KsVec3 z = ColorAt({0.f, 0.f, 1.f}, real);
        return {std::max(z.x * sky_mult.x, 0.001f) * L.colour.y, std::max(z.y * sky_mult.y, 0.001f) * L.colour.y, std::max(z.z * sky_mult.z, 0.0015f) * L.colour.y};
    }
};

} // namespace md

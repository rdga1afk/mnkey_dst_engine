#pragma once
// Kenshi's sky light on the CPU (#140 L1): the sun's path (SkyXController::update, Okran sky.rs F040798), the sun colour and strength (F040805), SkyX's atmospheric colour
// (getColorAt, HDR branch, SkyX_x64.dll 0x1800038e0, _scale 0x180003860), horizonClouds, zenithLight, pFogParams and MIN_LUMINANCE. Port of Project-Okran src/render/sky.rs (MIT).
// Constants of the GLOBAL CONSTANTS record 110-gamedata.quack (sunrise 5, sunset 23, latitude 54, exposure min 0.8 / max 1.2, night darkness 0.35: read from the install, same values).
#include <algorithm>
#include <cmath>

namespace md {

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
    float view_distance = 6000.f;         // the "View Distance" option (settings.cfg)
    KsVec3 clouds = {3.f, -0.8f, 0.f};    // cloud layer DensityMultiplier, DensityOffset, Darkness (F040804)
    KsVec3 sky_mult = {1.f, 1.f, 1.f};    // the weather's sky colour multiplier

    struct MainLight { KsVec3 dir; KsVec3 colour; float w = 0.f; float min_lum = 0.f; };

    static float Scale(float cos_a, float scale_depth) {
        const double x = (double)(1.0f - cos_a);
        const double e = (((x * 5.25 - 6.8) * x + 3.83) * x + 0.459) * x - 0.00287;
        return std::exp((float)e) * scale_depth;
    }
    static float Dot(KsVec3 a, KsVec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
    static KsVec3 Norm(KsVec3 a) { const float l = std::sqrt(Dot(a, a)); return l > 0.f ? KsVec3{a.x / l, a.y / l, a.z / l} : KsVec3{}; }

    // getColorAt, HDR lighting mode: the sky colour toward `direction` with the sun toward `sun` (both from the ground; RGB)
    KsVec3 ColorAt(KsVec3 direction, KsVec3 sun) const {
        const KenshiAtmosphere& A = atmosphere;
        const float inner = A.inner_radius, outer = A.outer_radius;
        const float scale_depth = (outer - inner) * 0.5f, scale_f = 1.f / (outer - inner), scale_over_depth = scale_f / scale_depth;
        const float pi = 3.14159265358979f;
        const float kr4pi = A.rayleigh * 4.f * pi, kr_esun = A.sun_intensity * A.rayleigh, km4pi = A.mie * 4.f * pi, km_esun = A.mie * A.sun_intensity;
        const KsVec3 dir = Norm(direction);
        const float cam_height = (outer - inner) * A.height_position + inner;
        const KsVec3 inv_wl = {1.f / std::pow(A.wavelength.x, 4.f), 1.f / std::pow(A.wavelength.y, 4.f), 1.f / std::pow(A.wavelength.z, 4.f)};
        const KsVec3 pos = {dir.x, dir.y + inner, dir.z};
        const KsVec3 start = {0.f, cam_height, 0.f};
        KsVec3 ray = {pos.x - start.x, pos.y - start.y, pos.z - start.z};
        const float far_d = std::sqrt(Dot(ray, ray));
        ray = {ray.x / far_d, ray.y / far_d, ray.z / far_d};
        const float depth0 = std::exp(scale_over_depth * (inner - cam_height));
        const float start_angle = Dot(ray, start) / cam_height;
        const float start_offset = depth0 * Scale(start_angle, scale_depth);
        const float sample_length = far_d / (float)A.samples, scaled_length = sample_length * scale_f;
        const KsVec3 sample_ray = {ray.x * sample_length, ray.y * sample_length, ray.z * sample_length};
        KsVec3 point = {start.x + sample_ray.x * 0.5f, start.y + sample_ray.y * 0.5f, start.z + sample_ray.z * 0.5f};
        KsVec3 front;
        for (int i = 0; i < std::max(A.samples, 0); ++i) {
            const float height = std::sqrt(Dot(point, point));
            const float depth = std::exp(scale_over_depth * (inner - height));
            const float light_angle = Dot(sun, point) / height, camera_angle = Dot(ray, point) / height;
            const float scatter = start_offset + depth * (Scale(light_angle, scale_depth) - Scale(camera_angle, scale_depth));
            const float k = depth * scaled_length;
            front.x += std::exp(-scatter * (inv_wl.x * kr4pi + km4pi)) * k;
            front.y += std::exp(-scatter * (inv_wl.y * kr4pi + km4pi)) * k;
            front.z += std::exp(-scatter * (inv_wl.z * kr4pi + km4pi)) * k;
            point = {point.x + sample_ray.x, point.y + sample_ray.y, point.z + sample_ray.z};
        }
        const KsVec3 rayleigh = {front.x * inv_wl.x * kr_esun, front.y * inv_wl.y * kr_esun, front.z * inv_wl.z * kr_esun};
        const KsVec3 mie = {front.x * km_esun, front.y * km_esun, front.z * km_esun};
        const KsVec3 to_cam = {start.x - pos.x, start.y - pos.y, start.z - pos.z};
        const float cs = Dot(sun, to_cam) / std::sqrt(Dot(to_cam, to_cam));
        const float g2 = A.g * A.g;
        const float ray_phase = (cs * cs * 0.5f + 1.f) * 0.75f;
        const float mie_phase = ((1.f - g2) / (g2 + 2.f)) * 1.5f * (cs * cs + 1.f) / std::pow(g2 + 1.f - 2.f * A.g * cs, 1.5f);
        KsVec3 c = {(rayleigh.x * ray_phase + mie.x * mie_phase) * A.exposure, (rayleigh.y * ray_phase + mie.y * mie_phase) * A.exposure, (rayleigh.z * ray_phase + mie.z * mie_phase) * A.exposure};
        // the night sky's glow toward the horizon
        const float m = std::max(c.x, std::max(c.y, c.z));
        const float fade = std::min(std::max(1.f - m * 10.f, 0.f), 1.f);
        const float below = std::min(std::max(-sun.y, 0.f), 1.f);
        const float k = 2.f - below * 0.75f;
        const float p = std::pow(1.f - dir.y, 3.f);
        c.x += fade * std::pow(k * 0.05f * p, 2.2f);
        c.y += fade * std::pow(k * 0.05f * p, 2.2f);
        c.z += fade * std::pow(k * 0.1f * p, 2.2f);
        return c;
    }

    // F040798: the sun's direction at an hour (0..24)
    KsVec3 SunDirection(float hour) const {
        float t = std::fmod(std::fmod(hour, 24.f) + 24.f, 24.f);
        float f;
        if (t < sunrise || sunset < t) {
            if (t < sunrise) t += 24.f;
            f = (t - sunset) / (24.f - (sunset - sunrise)) + 1.f;
        } else {
            f = (t - sunrise) / (sunset - sunrise);
        }
        const float a = f * 3.14159265358979f;
        return {std::cos(a), std::sin(a) * std::cos(tilt), std::sin(a) * std::sin(tilt)};
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

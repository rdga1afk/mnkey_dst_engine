#pragma once
// Kenshi auto-exposure (materials/post/hdrfp4.hlsl AdaptLuminance_fp4 / CalcExposedColor, hdrutils.hlsl): CPU model for the oracle of the GPU adaptation (#140 L4).
#include <algorithm>
#include <cmath>
namespace md {
constexpr float kKenshiExposureKey = 0.55f;        // EXPOSURE_KEY, hdr.material:7
constexpr float kKenshiAutoexpRate = 0.5f;         // AUTOEXP_ADAPTATION_RATE, hdr.material:6 (per second)
inline float KenshiAdaptLuminance(float last, float current, float dt, float rate, float lo, float hi) {
    const float a = last + (current - last) * (1.f - std::exp(-dt * rate));
    return std::min(std::max(a, lo), hi);
}
inline float KenshiExposureOf(float adapted_lum) { return kKenshiExposureKey / adapted_lum; }
}

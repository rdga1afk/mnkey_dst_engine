#pragma once
// FSR 2 parameters on the CPU (#156): quality modes, render size, jitter, mip bias. Formulas of AMD FidelityFX FSR 2 v2.2.1 (MIT, engine/third_party/fsr2/ffx-fsr2-api/ffx_fsr2.cpp:
// ffxFsr2GetUpscaleRatioFromQualityMode, ffxFsr2GetRenderResolutionFromQualityMode, ffxFsr2GetJitterPhaseCount, ffxFsr2GetJitterOffset, halton); the mip bias is the AMD recommendation (README.md of the FSR 2 repository, "Mipmap biasing": log2(renderResolution / displayResolution) - 1.0).
#include <cmath>
#include <cstdint>
namespace md {
enum class Fsr2Mode { Native = 0, Quality, Balanced, Performance, UltraPerformance };
inline float Fsr2UpscaleRatio(Fsr2Mode m) {
    switch (m) { case Fsr2Mode::Quality: return 1.5f; case Fsr2Mode::Balanced: return 1.7f; case Fsr2Mode::Performance: return 2.0f; case Fsr2Mode::UltraPerformance: return 3.0f; default: return 1.0f; }
}
inline void Fsr2RenderSize(Fsr2Mode m, uint32_t dw, uint32_t dh, uint32_t* rw, uint32_t* rh) {
    const float r = Fsr2UpscaleRatio(m);
    *rw = (uint32_t)((float)dw / r); *rh = (uint32_t)((float)dh / r);
}
inline int32_t Fsr2JitterPhaseCount(int32_t render_w, int32_t display_w) {
    return (int32_t)(8.0f * std::pow((float)display_w / (float)render_w, 2.0f));
}
inline float Fsr2Halton(int32_t index, int32_t base) {
    float f = 1.0f, result = 0.0f;
    for (int32_t i = index; i > 0;) { f /= (float)base; result += f * (float)(i % base); i = (int32_t)std::floor((float)i / (float)base); }
    return result;
}
// jitter in pixels of the render target, [-0.5, 0.5)
inline void Fsr2JitterOffset(int32_t index, int32_t phase_count, float* x, float* y) {
    *x = Fsr2Halton((index % phase_count) + 1, 2) - 0.5f; *y = Fsr2Halton((index % phase_count) + 1, 3) - 0.5f;
}
// texture lod bias while the scene is rendered at render_w for a display of display_w
inline float Fsr2MipBias(float render_w, float display_w) { return std::log2(render_w / display_w) - 1.0f; }
}

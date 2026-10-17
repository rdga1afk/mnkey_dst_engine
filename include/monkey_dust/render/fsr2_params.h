#pragma once
// FSR quality modes (#156): upscale ratio and render size for md.set_fsr1_mode. The ratios are those of AMD FidelityFX FSR 2 v2.2.1 (MIT) ffxFsr2GetUpscaleRatioFromQualityMode / ffxFsr2GetRenderResolutionFromQualityMode.
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
}

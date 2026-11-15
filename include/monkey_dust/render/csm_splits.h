// Cascade split points as the exe computes them (kenshi_x64.exe 1408638f0, call 140865150) = OGRE 1.9 PSSMShadowCameraSetup::calculateSplitPoints (OgreShadowCameraSetupPSSM.cpp:53-74)
// with an extra factor on the interior splits: split[0] = near, split[i] = scale * (lambda * near * (far / near)^(i / n) + (1 - lambda) * (near + (far - near) * i / n)) for 0 < i < n, split[n] = far.
// The exe passes n = 4, near = 1.0, lambda = 0.95, scale = 0.5, far = the `Shadow Range` option (docs/research/SUN_SHADOWS_EXE_2026-11-15.md).
#pragma once
#include <cmath>

namespace md::csm {
// out must hold n + 1 values.
inline void SplitPoints(int n, double near_d, double far_d, double lambda, double scale, double* out) {
    out[0] = near_d;
    for (int i = 1; i < n; ++i) {
        const double f = double(i) / double(n);
        out[i] = (lambda * near_d * std::pow(far_d / near_d, f) + (1.0 - lambda) * (near_d + (far_d - near_d) * f)) * scale;
    }
    out[n] = far_d;
}
} // namespace md::csm

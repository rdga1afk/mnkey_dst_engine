#pragma once
// Steering velocity update as the exe's character_steer_update (0x140147ca0); emulator vectors in tools/tests/data/exe_steer.tsv (tools_private/research/steer_oracle.py).
// wanted = direction * max speed (4 lanes); when the blocking query (0x1401476f0) reports a block (direction D, clearance C) and the wanted velocity points against D with
// cos < -0.8 (lanes x, y, z), a sideways 0.5 * (wanted.z, 0, -wanted.x, 0) is added, its sign from the side of D; then max(C, 0.01) * D is added. The change to the current
// velocity is limited to dt * acceleration (length over lanes x, y, z; all four lanes are scaled) and added. The advance of the +0x20 vector and the two helpers
// (0x140018bd8, 0x140009507) are not part of this function here. Not wired into the live game yet.
#include <algorithm>
#include <cmath>

namespace md::world {

struct SteerBlock {
    bool blocked = false;
    float dir[4] = {0, 0, 0, 0};        // D
    float clearance[4] = {0, 0, 0, 0};  // C
};

inline void SteerVelocity(float vel[4], const float dir[4], float max_speed, float accel, float dt, const SteerBlock& b = {}) {
    float v[4];
    for (int i = 0; i < 4; ++i) v[i] = max_speed * dir[i];
    auto dot3 = [](const float* p, const float* q) { return p[0] * q[0] + p[1] * q[1] + p[2] * q[2]; };
    if (b.blocked) {
        const float dp = dot3(b.dir, v);
        if (dp < 0.f) {
            const float n2 = dot3(v, v);
            const float cos_a = n2 > 0.f ? dp * (1.f / std::sqrt(n2)) : 0.f;
            if (cos_a < -0.8f) {
                const float perp[4] = {v[2], 0.f, -v[0], 0.f};
                const float s = dot3(perp, b.dir) >= 0.f ? 0.5f : -0.5f;
                for (int i = 0; i < 4; ++i) v[i] += s * perp[i];
            }
        }
        for (int i = 0; i < 4; ++i) v[i] += std::max(b.clearance[i], 0.01f) * b.dir[i];
    }
    float d[4];
    for (int i = 0; i < 4; ++i) d[i] = v[i] - vel[i];
    const float len = std::sqrt(dot3(d, d));
    const float limit = dt * accel;
    const float scale = (limit < len) ? limit / len : 1.f;
    for (int i = 0; i < 4; ++i) vel[i] += d[i] * scale;
}

} // namespace md::world

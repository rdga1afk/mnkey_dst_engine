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

struct Neighbour {
    float pos[3];      // game units (the exe multiplies by 0.1)
    float radius;      // game units
    bool valid;        // alive, not hidden, passes the three flag tests of the exe loop
};

struct AvoidResult {
    int status = 0;                 // 0 nothing pushes (exe returns 0), 1 a neighbour pushes, 2 a neighbour sits on the character (the exe then draws a random direction: not ported)
    float dir[4] = {0, 0, 0, 0};
    float strength = 0.f;
    int winner = -1;
    int state = 0;                  // the character's +0x88 after the call (2 -> 1 when the target lies within the winner's radius)
};

// character_neighbour_avoidance_query (0x1401476f0); emulator vectors in tools/tests/data/exe_neighbour.tsv (tools_private/research/query_oracle.py). Horizontal only: the neighbour's y is the character's y.
// A neighbour overlaps when the squared horizontal distance is below (own radius + 0.1 * its radius)^2; its strength is (sum - distance) * (0.5 + (0.1 * its radius - own radius) / sum), clamped to 0..strength_cap
// (the exe's cap, a global written by a static constructor, is 1.0). The strongest wins (strict >); the best strength below 0.001 means no push; with body_flag (the character's body virtual +0x368) it is scaled by 0.02.
inline AvoidResult NeighbourAvoidance(const float cpos[4], float own_radius, int state, const float target[3], const Neighbour* nb, int count, bool body_flag, float strength_cap = 1.0f) {
    AvoidResult r; r.state = state;
    for (int i = 0; i < count; ++i) {
        if (!nb[i].valid) continue;
        const float nx = (float)((double)nb[i].pos[0] * 0.1), nz = (float)((double)nb[i].pos[2] * 0.1);
        if (std::fabs(nx - cpos[0]) <= 0.001f && std::fabs(nz - cpos[2]) <= 0.001f) { r.status = 2; r.winner = i; return r; }
        const float sum = (float)((double)nb[i].radius * 0.1 + (double)own_radius);
        const float dx = cpos[0] - nx, dz = cpos[2] - nz, d2 = dx * dx + dz * dz;
        if (!(d2 < sum * sum)) continue;
        const float inv = d2 > 0.f ? 1.f / std::sqrt(d2) : 0.f;
        float st = (sum - d2 * inv) * ((float)((double)nb[i].radius * 0.1 - own_radius) * (1.f / sum) + 0.5f);
        st = std::max(0.f, std::min(st, strength_cap));
        if (st > r.strength) { r.strength = st; r.winner = i; r.dir[0] = dx * inv; r.dir[1] = 0.f; r.dir[2] = dz * inv; r.dir[3] = cpos[3] * inv; }
        if (r.state == 2) {
            const float tx = nx - target[0], tz = nz - target[2];
            if (tx * tx + tz * tz < std::pow((float)((double)nb[i].radius * 0.1), 2.0f)) r.state = 1;
        }
    }
    if (r.strength < 0.001f) { r.status = 0; r.winner = -1; for (float& d : r.dir) d = 0.f; r.strength = 0.f; return r; }
    if (body_flag) r.strength *= 0.02f;
    r.status = 1;
    return r;
}

} // namespace md::world

#pragma once
// The squad of the Q04 vertical slice (#212): selection (player_selection.h), a move order along a waypoint path, per-tick movement from the exe rules already ported
// (MoveSpeedCap, NeighbourAvoidance = 0x1401476f0, SteerVelocity = 0x140147ca0). Pure data and arithmetic, fixed arrays, no game objects: the game layer (MD_SLICE=1) feeds
// clicks and a path from the install navmesh, reads positions back, and a replay of N ticks has one state hash.
// DEVIATION: the exe's acceleration field (+0x78) and the unit of the speed caps are not read; a cap is taken as game units per second (0.1 m per unit, the factor the exe's
// neighbour query applies to positions), the acceleration is kAccel; the coincident neighbour case (random direction in the exe) uses a fixed sideways push.
#include <cmath>
#include <cstdint>
#include <monkey_dust/world/move_speed.h>
#include <monkey_dust/world/player_selection.h>
#include <monkey_dust/world/steer.h>

namespace md::world {

constexpr int kSliceMaxChars = 32, kSliceMaxWaypoints = 64;
constexpr float kSliceAccel = 40.f;        // m/s^2, placeholder (see DEVIATION)
constexpr float kSliceArrive = 0.3f;       // m

struct SliceChar {
    float pos[4] = {0, 0, 0, 0};           // metres, w unused
    float vel[4] = {0, 0, 0, 0};           // m/s
    float radius = 0.5f;                   // metres
    float walk_cap = kWalkCapDefault;      // game units per second
    float cap = kWalkCapDefault;
    bool moving = false;
    int wp = 0, nwp = 0;
    float wps[kSliceMaxWaypoints][3] = {};
};

struct SliceSquad {
    SliceChar c[kSliceMaxChars];
    int n = 0;
    PlayerSelection sel;                   // ids are index + 1
    int Add(float x, float y, float z) {
        if (n >= kSliceMaxChars) return -1;
        c[n].pos[0] = x; c[n].pos[1] = y; c[n].pos[2] = z;
        return n++;
    }
    bool Click(int i, bool add) {          // a click on character i; without `add` the set becomes {i} (select first: the last selected cannot be deselected)
        if (i < 0 || i >= n) return false;
        const PlayerSelection::Pos p{c[i].pos[0], c[i].pos[1], c[i].pos[2]};
        if (add) return sel.Has(i + 1) ? sel.Deselect(i + 1) : sel.Select(i + 1, p);
        bool changed = sel.Select(i + 1, p);
        for (int k = 0; k < n; ++k) if (k != i) changed |= sel.Deselect(k + 1);
        return changed;
    }
    // order every selected character to follow `pts` (npts >= 1) with the speed mode of setMoveSpeed
    void OrderMove(const float (*pts)[3], int npts, int mode) {
        if (npts < 1) return;
        if (npts > kSliceMaxWaypoints) npts = kSliceMaxWaypoints;
        for (int i = 0; i < n; ++i) {
            if (!sel.Has(i + 1)) continue;
            SliceChar& ch = c[i];
            ch.cap = MoveSpeedCap(mode, ch.cap, ch.walk_cap);
            for (int k = 0; k < npts; ++k) for (int a = 0; a < 3; ++a) ch.wps[k][a] = pts[k][a];
            ch.nwp = npts; ch.wp = 0; ch.moving = true;
        }
    }
};

inline void SliceTick(SliceSquad& s, float dt) {
    Neighbour nb[kSliceMaxChars];
    for (int i = 0; i < s.n; ++i) {
        SliceChar& ch = s.c[i];
        if (!ch.moving) continue;
        const float* t = ch.wps[ch.wp];
        float dx = t[0] - ch.pos[0], dz = t[2] - ch.pos[2], dist = std::sqrt(dx * dx + dz * dz);
        while (dist < kSliceArrive && ch.moving) {
            if (++ch.wp >= ch.nwp) { ch.moving = false; ch.wp = ch.nwp - 1; for (float& v : ch.vel) v = 0.f; break; }
            t = ch.wps[ch.wp]; dx = t[0] - ch.pos[0]; dz = t[2] - ch.pos[2]; dist = std::sqrt(dx * dx + dz * dz);
        }
        if (!ch.moving) continue;
        const float dir[4] = {dx / dist, 0.f, dz / dist, 0.f};
        int cnt = 0;
        for (int j = 0; j < s.n; ++j) if (j != i) nb[cnt++] = {{s.c[j].pos[0] * 10.f, s.c[j].pos[1] * 10.f, s.c[j].pos[2] * 10.f}, s.c[j].radius * 10.f, true};
        const float target[3] = {t[0], t[1], t[2]};
        const AvoidResult r = NeighbourAvoidance(ch.pos, ch.radius, 0, target, nb, cnt, false);
        SteerBlock b;
        if (r.status == 1) { b.blocked = true; for (int k = 0; k < 4; ++k) { b.dir[k] = r.dir[k]; b.clearance[k] = r.strength; } }
        else if (r.status == 2) { b.blocked = true; b.dir[0] = 0.f; b.dir[2] = 1.f; for (float& v : b.clearance) v = 0.5f; }
        SteerVelocity(ch.vel, dir, ch.cap * 0.1f, kSliceAccel, dt, b);
        ch.pos[0] += ch.vel[0] * dt; ch.pos[2] += ch.vel[2] * dt; ch.pos[1] = t[1];
    }
}

// FNV-1a over positions and velocities rounded to 1 mm / 1 mm/s, the moving flags and the selection count: one value per replay state
inline uint64_t SliceStateHash(const SliceSquad& s) {
    uint64_t h = 1469598103934665603ull;
    auto mix = [&](int64_t v) { for (int k = 0; k < 8; ++k) { h ^= (uint64_t)((v >> (8 * k)) & 0xff); h *= 1099511628211ull; } };
    mix(s.n); mix(s.sel.Count());
    for (int i = 0; i < s.n; ++i) {
        for (int a = 0; a < 3; ++a) { mix((int64_t)std::llround(s.c[i].pos[a] * 1000.0)); mix((int64_t)std::llround(s.c[i].vel[a] * 1000.0)); }
        mix(s.c[i].moving ? s.c[i].wp + 1 : 0);
    }
    return h;
}

} // namespace md::world

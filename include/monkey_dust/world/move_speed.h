#pragma once
// Movement speed mode cap as the exe's setMoveSpeed (0x140665150) sets it; emulator vectors in tools_private/research/exe_oracle_vectors3.tsv.
// Mode 0 (walk) takes the character's own walk cap (the exe's field +0xc0, 15.0 from the constructor); 1 (jog) 55.0; 2 and 3 (run) 999.0;
// any other mode leaves the current cap unchanged. Not wired into the live game yet (NavAgent keeps its fixed speeds).

namespace md::world {

constexpr float kWalkCapDefault = 15.f;
constexpr float kJogCap = 55.f;
constexpr float kRunCap = 999.f;

inline float MoveSpeedCap(int mode, float current, float walk_cap = kWalkCapDefault) {
    switch (mode) {
        case 0: return walk_cap;
        case 1: return kJogCap;
        case 2: case 3: return kRunCap;
        default: return current;
    }
}

} // namespace md::world

#pragma once
#include <cstdint>
#if defined(MD_ECS_GAIA)
#include <gaia.h>
#endif

// Перейменовано в WorldTransform — raylib вже має свій тип Transform.
// Базовий engine-рівень просторовий компонент; використовується
// TransformSoA і SpatialGrid (обидва в engine/).
//
// GAIA_STORAGE(Sparse) — Phase 2 (prompt_/PROMPT_GAIA_MIGRATION.md §0/§4):
// one of the 5 components with a proven B3.4 risk (T& held across a
// structural change of the SAME entity, then dereferenced -- gaia's
// Sparse storage gives payload a stable address across archetype moves,
// closing this mechanically instead of by convention).
struct WorldTransform {
#if defined(MD_ECS_GAIA)
    GAIA_STORAGE(Sparse);
#endif
    float    x, y, z;
    float    rot_y;      // тільки поворот по Y (top-down RPG)
    uint32_t slot = 0xFFFFFFFFu; // TransformSoA slot index
};

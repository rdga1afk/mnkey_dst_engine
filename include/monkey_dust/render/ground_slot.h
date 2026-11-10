#pragma once
// Ground live pass (default off, MD_GROUND_LIVE=1): the terrain G-buffer pass gets a second colour target (R8_UINT) that holds the biome-list slot of each patch.
// Every pipeline that draws in that pass then declares the second target, so the switch is read once when the pipelines are created.
#include <cstdlib>

namespace md {
inline bool GroundSlotTargetOn() { static const bool on = getenv("MD_GROUND_LIVE") && getenv("MD_GROUND_LIVE")[0] == '1'; return on; }
// MD_GROUND_LIVE_LIGHT=1 (with MD_GROUND_LIVE=1): the non-reuse ground resolve variants read the live ground material (albedo + gloss, normal, distance) from two extra textures (slots 2 and 15) where the distance is not 0.
inline bool GroundLiveLightOn() { static const bool on = GroundSlotTargetOn() && getenv("MD_GROUND_LIVE_LIGHT") && getenv("MD_GROUND_LIVE_LIGHT")[0] == '1'; return on; }
}   // namespace md

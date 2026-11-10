#pragma once
// Ground live pass (default off, MD_GROUND_LIVE=1): the terrain G-buffer pass gets a second colour target (R8_UINT) that holds the biome-list slot of each patch.
// Every pipeline that draws in that pass then declares the second target, so the switch is read once when the pipelines are created.
#include <cstdlib>

namespace md {
inline bool GroundSlotTargetOn() { static const bool on = getenv("MD_GROUND_LIVE") && getenv("MD_GROUND_LIVE")[0] == '1'; return on; }
}   // namespace md

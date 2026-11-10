#pragma once
// Ground live pass (default off, MD_GROUND_LIVE=1): the terrain G-buffer pass gets a second colour target (R8_UINT) that holds the biome-list slot of each patch.
// Every pipeline that draws in that pass then declares the second target, so the switch is read once when the pipelines are created.
#include <cstdlib>

namespace md {
inline bool GroundSlotTargetOn() { static const bool on = getenv("MD_GROUND_LIVE") && getenv("MD_GROUND_LIVE")[0] == '1'; return on; }
// MD_GROUND_LIVE_LIGHT=1 (with MD_GROUND_LIVE=1): the non-reuse ground resolve variants read the live ground material (albedo + gloss, normal, distance) from two extra textures (slots 2 and 15) where the distance is not 0.
inline bool GroundLiveLightOn() { static const bool on = GroundSlotTargetOn() && getenv("MD_GROUND_LIVE_LIGHT") && getenv("MD_GROUND_LIVE_LIGHT")[0] == '1'; return on; }
// Run-time switches (Lua md.set_ground_live_switch), env var = initial value; -1 = not yet read. One game process can serve several arms that differ only in these.
// MD_GROUND_LIVE and MD_GROUND_LIVE_LIGHT stay start-up only (they decide the G-buffer targets and pipelines).
struct GroundLiveRt { int orig_light = -1, tiny = -1, groups = -1; };
inline GroundLiveRt& GroundLiveRtState() { static GroundLiveRt r; return r; }
inline bool GroundLiveEnv1(const char* n) { const char* e = getenv(n); return e && e[0] == '1'; }
inline bool GroundLiveTinyOn() { int& v = GroundLiveRtState().tiny; if (v < 0) v = GroundLiveEnv1("MD_GROUND_LIVE_TINY") ? 1 : 0; return v == 1; }
inline bool GroundLiveOrigLightOn() { int& v = GroundLiveRtState().orig_light; if (v < 0) v = GroundLiveEnv1("MD_GROUND_LIVE_ORIGLIGHT") ? 1 : 0; return v == 1; }
}   // namespace md

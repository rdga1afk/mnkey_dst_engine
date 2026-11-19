// Water constants of the exe (water_system_init 0x1409d70f0 and the second write at 0x1409d89f1 in 0x1409d7880): the vertex constant mapBounds = (-147456, -147456, 147456, 147456) in 0.1 m units, the extent of the flow / colour maps.
#pragma once

namespace md {

constexpr float kWaterMapBoundsUnits = 147456.f;   // exe data 0x141693efc (+147456.0) and 0x1416c9fb0 (-147456.0)

// The span of the maps in metres (our world coordinates start at the world corner, so the exe bounds become (0, 0, span, span)).
constexpr float WaterMapSpanMetres() { return 2.f * kWaterMapBoundsUnits / 10.f; }

}   // namespace md

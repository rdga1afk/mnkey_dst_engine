#pragma once
// The terrain material's per-biome shader inputs, packed from one BIOMES record like the exe's terrain material builder does (private notes,
// ground packer): the full set the original terrain fragment program reads (all four slope bands, six absorbances, the colour fade).
// A field the record does not hold reads 0, like the original's map lookup.
#include <monkey_dust/io/fcs.h>

namespace md {

struct GroundPack {
    float scalesA[4];       // tiling X 1, Y 1, X 2, Y 2
    float scalesB[4];       // tiling X 0, Y 0, X grass, Y grass
    float scalesC[4];       // tiling X dirt, Y dirt, X road, Y road
    float slopeMin[4];      // slope min 1, 2, 3, grass  (x 0.01)
    float slopeMax[4];
    float slopeBlend[4];    // slope fade 1, 2, 3, grass (x 0.01)
    float overlayMult[4];   // vertical, grass, dirt, road
    float textureFade[4];   // ground colour r, g, b (from ARGB), 1 / fade distance (the raw value when it is not above 0)
    float absorbance[6];    // 0, 1, 2, grass, dirt, road
    float brightnessFix;    // 0 reads as 1
    float distortFreq;      // 1 / distort wavelength, 0 when the wavelength is not above 0
    float distortAmp;       // distort amplitude x 0.01
};

GroundPack PackGroundBiome(const fcs::Record& r);

}   // namespace md

#include <monkey_dust/world/ground_pack.h>

namespace md {
namespace {
float Num(const fcs::Record& r, const char* key) {
    for (const auto& kv : r.floats) if (kv.first == key) return kv.second;
    for (const auto& kv : r.ints) if (kv.first == key) return (float)kv.second;
    return 0.f;
}
bool Int(const fcs::Record& r, const char* key, int32_t& out) {
    for (const auto& kv : r.ints) if (kv.first == key) { out = kv.second; return true; }
    return false;
}
}   // namespace

GroundPack PackGroundBiome(const fcs::Record& r) {
    GroundPack p{};
    const char* ax[4] = { "tiling X 1", "tiling Y 1", "tiling X 2", "tiling Y 2" };
    const char* bx[4] = { "tiling X 0", "tiling Y 0", "tiling X grass", "tiling Y grass" };
    const char* cx[4] = { "tiling X dirt", "tiling Y dirt", "tiling X road", "tiling Y road" };
    const char* mn[4] = { "slope min 1", "slope min 2", "slope min 3", "slope min grass" };
    const char* mx[4] = { "slope max 1", "slope max 2", "slope max 3", "slope max grass" };
    const char* fd[4] = { "slope fade 1", "slope fade 2", "slope fade 3", "slope fade grass" };
    const char* om[4] = { "overlay mult vertical", "overlay mult grass", "overlay mult dirt", "overlay mult road" };
    const char* ab[6] = { "absorbance 0", "absorbance 1", "absorbance 2", "absorbance grass", "absorbance dirt", "absorbance road" };
    for (int i = 0; i < 4; ++i) {
        p.scalesA[i] = Num(r, ax[i]); p.scalesB[i] = Num(r, bx[i]); p.scalesC[i] = Num(r, cx[i]);
        p.slopeMin[i] = Num(r, mn[i]) * 0.01f; p.slopeMax[i] = Num(r, mx[i]) * 0.01f; p.slopeBlend[i] = Num(r, fd[i]) * 0.01f;
        p.overlayMult[i] = Num(r, om[i]);
    }
    for (int i = 0; i < 6; ++i) p.absorbance[i] = Num(r, ab[i]);
    int32_t g = 0; Int(r, "ground colour", g);
    const uint32_t u = (uint32_t)g;
    p.textureFade[0] = (float)((u >> 16) & 255u) / 255.f; p.textureFade[1] = (float)((u >> 8) & 255u) / 255.f; p.textureFade[2] = (float)(u & 255u) / 255.f;
    const float fade = Num(r, "fade distance");
    p.textureFade[3] = fade > 0.f ? 1.f / fade : fade;
    p.brightnessFix = Num(r, "brightness fix"); if (p.brightnessFix == 0.f) p.brightnessFix = 1.f;
    const float wl = Num(r, "distort wavelength");
    p.distortFreq = wl > 0.f ? 1.f / wl : 0.f;
    p.distortAmp = Num(r, "distort amplitude") * 0.01f;
    return p;
}

}   // namespace md

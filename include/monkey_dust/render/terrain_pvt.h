#pragma once
#include <cstdlib>
// #207 PVT stage 1 (docs/research/TERRAIN_PVT_DESIGN_2026-10-28.md): a cache of baked ground material tiles. A tile is 128 x 128 texels (2 border texels on each side) of one mip level:
// mip m has the texel size 0.02 m * 2^m, so a screen pixel at the distance d (footprint d * pix_angle) takes the mip whose texel is nearest to it. The tile (mip, ti, tj) covers
// the 124 * texel square [ti, ti + 1) * 124 * texel of the absolute ground plane. Albedo + gloss (RGBA8 sRGB) and the tangent-space normal xy (RG8) live in two texture arrays (the atlas); a toroidal
// page table (24 x 24 tiles per mip, RGBA16UI: layer + 1, ti & 0xffff, tj & 0xffff) maps a ground position to its layer. CPU side: which tiles the view needs, and the layer allocation.
#include <SDL3/SDL_gpu.h>
#include <cstdint>

class TerrainPvt {
public:
    static constexpr int   kMips = 10, kRing = 24, kTexels = 128, kBorder = 2, kInner = kTexels - 2 * kBorder;
    static float Finest() { static const float f = [] { const char* e = getenv("MD_PVT_FINEST"); return e ? (float)atof(e) : 0.02f; }(); return f; }   // m, texel of mip 0 (0.02 by default; MD_PVT_FINEST is a measurement knob and needs the shader built with the same -DTS_PVT_FINEST)
    static constexpr float kRefPixAngle = 0.0010690f;           // 2 tan(30 deg) / 1080: the pixel angle the tile content (distance fades) is baked for
    // true and the height (absolute m) when the ground exists at (x, z)
    using HeightFn = bool (*)(float x, float z, float& h);
    struct Bake { int mip, ti, tj, layer; };
    struct Stats { int resident = 0, wanted = 0, missing = 0, baked_total = 0, layers = 0; };

    bool Init(SDL_GPUDevice* dev, int megabytes);
    void Shutdown(SDL_GPUDevice* dev);
    bool Ready() const { return albedo_ && normal_ && table_; }

    // One frame of planning: walks the ground in the view, marks the wanted tiles, returns up to max_out tiles to bake (nearest first) with layers assigned and the page table updated.
    // inv_vp: column-major inverse view-projection; fine_min / fine_max: the part of the ground (absolute m) the tile bake may read (the fine normal window without its rim).
    int Plan(const float cam[3], const float inv_vp[16], float pix_angle, HeightFn hf, const float fine_min[2], const float fine_max[2], uint32_t frame, Bake* out, int max_out);
    // Inside no pass: uploads the page table when it changed.
    void UploadTable(SDL_GPUDevice* dev, SDL_GPUCommandBuffer* cmd);
    void Flush();                                               // forget every tile (the table is cleared; the atlas keeps stale data nobody can reach)
    const Stats& GetStats() const { return stats_; }

    SDL_GPUTexture* Albedo() const { return albedo_; }
    SDL_GPUTexture* Normal() const { return normal_; }
    SDL_GPUTexture* Table() const { return table_; }
    SDL_GPUSampler* Linear() const { return lin_; }
    SDL_GPUSampler* Nearest() const { return nearest_; }
    int  Layers() const { return layers_; }
    int  Resident() const { return resident_; }
    static bool GradMip() { static const bool g = [] { const char* e = getenv("MD_PVT_GRADMIP"); return e && e[0] == '1'; }(); return g; }   // measurement switch: tile level from the XZ screen gradients (Dagor formula); the shader needs -DTS_PVT_GRADMIP
    static float TexelOf(int mip) { return Finest() * (float)(1 << mip); }

private:
    struct Entry { uint16_t layer1, ti, tj, pad; };
    SDL_GPUTexture* albedo_ = nullptr; SDL_GPUTexture* normal_ = nullptr; SDL_GPUTexture* table_ = nullptr;
    SDL_GPUSampler* lin_ = nullptr; SDL_GPUSampler* nearest_ = nullptr;
    SDL_GPUTransferBuffer* tb_ = nullptr;
    int layers_ = 0, resident_ = 0, free_top_ = 0;
    bool dirty_ = true;
    Entry*    table_cpu_ = nullptr;       // kMips * kRing * kRing
    int32_t*  layer_slot_ = nullptr;      // layer -> table index, -1 = free
    uint32_t* layer_seen_ = nullptr;      // layer -> the last frame the tile was wanted
    int32_t*  free_ = nullptr;
    Stats stats_;
};

#pragma once
// RdTexture — GPU texture with automatic mipmap generation and Intel-safe sampler.
// Replaces ad-hoc SDL_GPUTexture creation in main.cpp.
// Mip chain: num_levels = floor(log2(max(w,h)))+1 when gen_mipmaps=true.
// Sampler: LINEAR min+mag, LINEAR_MIPMAP when mips>1, NEAREST when single-mip
//          (Intel ANV HD 520 returns (0,1,0,1) sentinel with LINEAR on single-mip).
#include <SDL3/SDL_gpu.h>
#include <monkey_dust/render/gpu_device.h>
#include <cstdint>

namespace md {

class RdTexture {
public:
    struct Desc {
        uint32_t             width      = 0;
        uint32_t             height     = 0;
        SDL_GPUTextureFormat format     = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
        bool                 gen_mipmaps = true;
    };

    // Upload raw RGBA8 pixel data. Creates texture + sampler.
    // Returns false and leaves object in invalid state on failure.
    bool Upload(const uint8_t* rgba8, const Desc& d);

    // Load from file via stb_image, then Upload(). Flip V for GL convention.
    bool LoadFile(const char* path, bool gen_mipmaps = true, bool flip_v = false);

    md::GpuTextureHandle              Tex()     const { return tex_; }
    SDL_GPUSampler*              Samp()    const { return samp_; }
    SDL_GPUTextureSamplerBinding Binding() const { return {tex_, samp_}; }
    bool                         Valid()   const { return tex_ != nullptr; }
    uint32_t                     MipLevels() const { return mip_levels_; }

    void Release();

private:
    md::GpuTextureHandle tex_        = nullptr;
    SDL_GPUSampler* samp_       = nullptr;
    uint32_t        mip_levels_ = 1;
};

} // namespace md

#pragma once
// RdPipeline — GpuPipeline with Intel ANV limit validation.
// Clamps frag_samplers and vert_storage_bufs to RdDevice::Caps() before creation.
// Logs a warning for each clamped resource count.
#ifdef MD_SDL_GPU
#include <monkey_dust/render/gpu_hal_types.h>
#include <monkey_dust/render/gpu_pipeline.h>
#include <SDL3/SDL_gpu.h>

namespace md {

class RdPipeline {
public:
    struct Desc {
        const char*          vert_path         = nullptr;
        const char*          frag_path         = nullptr;
        GpuVertexLayout      layout            = {};
        GpuRasterState       raster            = {};
        uint32_t             vert_uniform_bufs = 0;
        uint32_t             vert_storage_bufs = 0;
        uint32_t             frag_uniform_bufs = 0;
        uint32_t             frag_samplers     = 0;
        SDL_GPUTextureFormat color_format      = SDL_GPU_TEXTUREFORMAT_INVALID;
        bool                 has_depth_target  = true;
        bool                 depth_only        = false;
    };

    // Validates d against RdDevice::Caps(), clamps if needed, then creates pipeline.
    bool Create(const Desc& d);
    void Destroy();

    SDL_GPUGraphicsPipeline* SDLPipeline() const { return gp_.SDLPipeline(); }
    bool Valid() const { return gp_.SDLPipeline() != nullptr; }

private:
    GpuPipeline gp_;
};

} // namespace md
#endif // MD_SDL_GPU

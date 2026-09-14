#pragma once
// gpu_depth_texture.h — GpuDepthTexture (split from gpu_hal.h, code audit
// proposal #8).

#include <cstdint>

#ifdef MD_SDL_GPU
#include <SDL3/SDL_gpu.h>
#endif

// ─────────────────────────────────────────────────────────────────────────────
// GpuDepthTexture — depth texture for shadow passes.
// OpenGL: FBO + GL_DEPTH_COMPONENT24 texture.
// SDL_GPU: SDL_GPUTexture(D32_FLOAT, DEPTH_STENCIL_TARGET|SAMPLER) — no FBO.
//   D32_FLOAT, not D24_UNORM: Intel Gen9 (HD 520) cannot sample D24_UNORM
//   (see gpu_hal_buffers.cpp's Init() for the same note at the real call site).
//   shadow_border: GL uses CLAMP_TO_BORDER (white); SDL_GPU uses CLAMP_TO_EDGE
//   (SDL3 has no border color support — minor edge artifact, acceptable).
// ─────────────────────────────────────────────────────────────────────────────
class GpuDepthTexture {
public:
    void Init(int w, int h, bool shadow_border = false);
    void Shutdown();

    // OpenGL: glActiveTexture + glBindTexture.
    // SDL_GPU: binding done via SDL_BindGPUFragmentSamplers in render pass (Step 6).
    void Bind(uint32_t unit) const;

    unsigned int FBO()     const { return fbo_; }   // 0 in SDL_GPU path
    unsigned int Texture() const { return tex_; }   // 0 in SDL_GPU path
    int Width()  const { return w_; }
    int Height() const { return h_; }
#ifdef MD_SDL_GPU
    // TEMPORARY raw-SDL bridge, same idiom as GpuDevice::SDLDevice() --
    // see its comment. Do not add new call sites.
    SDL_GPUTexture*  SDLTexture() const { return sdl_tex_; }
    SDL_GPUSampler*  SDLSampler() const { return sdl_sampler_; }
#endif

private:
    unsigned int fbo_ = 0;
    unsigned int tex_ = 0;
    int w_ = 0, h_ = 0;
#ifdef MD_SDL_GPU
    SDL_GPUTexture* sdl_tex_     = nullptr;
    SDL_GPUSampler* sdl_sampler_ = nullptr;
#endif
};

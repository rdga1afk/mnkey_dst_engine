#pragma once
// CasPass — Contrast Adaptive Sharpening post-process pass.
//
// Usage:
//   CasPass cas;
//   cas.Init(vp_w, vp_h);               // once after GpuDevice::Init
//
//   // Per frame:
//   SDL_GPUTexture* scene_tex = cas.SceneTex();  // render scene into this
//   cas.Apply(cmd, swapchain_tex, vp_w, vp_h);   // then apply CAS → swap
//
//   cas.Resize(vp_w, vp_h);             // on window resize
//   cas.Shutdown();                      // at exit

#ifdef MD_SDL_GPU
#include <SDL3/SDL_gpu.h>
#include <monkey_dust/render/gpu_pipeline.h>
namespace md {

class CasPass {
public:
    // sharpness [0..1]: 0 = no effect (pass-through), 1 = maximum sharpening.
    bool Init(int vp_w, int vp_h, float sharpness = 0.5f);
    void Shutdown();

    // Recreate scene texture if viewport changed.
    void Resize(int vp_w, int vp_h);

    // The intermediate texture to render the scene into.
    // Usage: render into SceneTex(), then call Apply().
    md::GpuTextureHandle SceneTex() const { return scene_tex_; }
    // Optional depth texture for the scene render pass.
    md::GpuTextureHandle DepthTex() const { return depth_tex_; }

    bool IsReady() const { return ready_; }
    float Sharpness() const { return sharpness_; }
    void  SetSharpness(float s) { sharpness_ = s < 0.f ? 0.f : s > 1.f ? 1.f : s; }

    // Apply CAS: reads SceneTex(), writes sharpened result to output_tex.
    // cmd: the current frame command buffer (before Submit).
    // output_tex: typically the swapchain texture.
    void Apply(md::GpuCommandBufferHandle cmd, md::GpuTextureHandle output_tex,
               int vp_w, int vp_h);

private:
    void CreateTextures(int w, int h);
    void DestroyTextures();

    GpuPipeline     pipeline_;
    md::GpuTextureHandle scene_tex_  = nullptr;
    SDL_GPUSampler* sampler_    = nullptr;
    md::GpuTextureHandle depth_tex_  = nullptr;

    float sharpness_ = 0.5f;
    int   tex_w_     = 0;
    int   tex_h_     = 0;
    bool  ready_     = false;
};

} // namespace md
#endif // MD_SDL_GPU

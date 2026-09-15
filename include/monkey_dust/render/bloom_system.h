#pragma once
#ifdef MD_SDL_GPU
#include <SDL3/SDL_gpu.h>
#include <monkey_dust/render/gpu_pipeline.h>
#include <monkey_dust/render/gpu_sampler_texture.h>
#include <monkey_dust/render/gpu_pass_view.h>
// ── BloomSystem (VBfA-R4) ─────────────────────────────────────────────────────
// HDR Bloom: 4×4 downsample with luma threshold + Gaussian5×5 separable blur.
//
// Architecture — requires an intermediate scene color RT (not swapchain):
//   1. Scene renders into scene_color_ (RGBA16F, full-res).
//   2. ComputePass(cmd): bloom_src_ ← downsample+threshold(scene_color_)
//                        bloom_temp_ ← Gaussian5H(bloom_src_)
//                        bloom_out_  ← Gaussian5V(bloom_temp_)
//   3. CompositePass(cmd, swapchain, w, h): scene_color_ + bloom_out_ → swapchain
//
// Scene render integration in main.cpp:
//   if (BloomSystem::Get().IsEnabled()) {
//       // Open render pass manually on SceneColorTex() instead of swapchain
//       SDL_BeginGPURenderPass(cmd, SceneColorTargetInfo(), ...)
//   } else {
//       GpuRenderPass::BeginColor(...)   // → swapchain, as before
//   }
//
// Gate: RenderTierSystem::IsDeferred() → Deferred_Low+.
// VBfA reference: filter_Sampled4x4 + PS_Gaussian5Horiz/Vert + frame_buffer_postprocess.

namespace md {

class BloomSystem {
public:
    static BloomSystem& Get();

    // Allocate textures + pipelines. No-op if !IsDeferred().
    void Init(md::GpuDeviceHandle dev, int w, int h);
    void Shutdown();

    // Scene color RT — render the entire scene here (instead of swapchain).
    md::GpuTextureHandle SceneColorTex()     const { return scene_color_.SDLTexture(); }
    SDL_GPUSampler* SceneColorSampler() const { return scene_sampler_.SDLSampler(); }

    // Helper: fill a SDL_GPUColorTargetInfo for the scene color RT (CLEAR).
    SDL_GPUColorTargetInfo SceneColorTargetInfo() const;

    // Downsample → H blur → V blur. Call after scene + SSAO on scene_color_.
    void ComputePass(md::GpuCommandBufferHandle cmd);

    // Blit scene_color_ + bloom → swapchain_tex (open CLEAR pass internally).
    void CompositePass(md::GpuCommandBufferHandle cmd,
                       md::GpuTextureHandle swapchain_tex, int sw, int sh);

    bool IsEnabled() const { return enabled_; }
    int  Width()  const { return w_; }
    int  Height() const { return h_; }

    float bloom_threshold = 0.9f;   // luma cut-off
    float bloom_intensity = 0.25f;  // additive bloom scale
    float exposure        = 1.0f;   // scene exposure
    float gamma           = 2.2f;   // output gamma

    // VBfA-R5: colour grade polynomial eq[0..3] (vec4, RGB+luminance each).
    // Formula: result = eq[3].rgb
    //                 + (eq[2].rgb + eq[2].a) * t
    //                 + (eq[1].rgb + eq[1].a) * t²
    //                 + (eq[0].rgb + eq[0].a) * t³
    // Neutral: eq[2].a=1, all else 0.  Applied before gamma correction.
    float grade_eq[4][4] = {
        {0.f, 0.f, 0.f, 0.f},   // cubic  (eq[0])
        {0.f, 0.f, 0.f, 0.f},   // quadratic (eq[1])
        {0.f, 0.f, 0.f, 1.f},   // linear  (eq[2]) — .a=1 = identity
        {0.f, 0.f, 0.f, 0.f},   // constant (eq[3])
    };

    // Apply a named biome grade preset.
    void SetBiomeGrade(const char* biome);

private:
    BloomSystem() = default;

    md::GpuDeviceHandle  dev_          = nullptr;
    bool            enabled_      = false;
    int w_ = 0, h_ = 0;    // full-res
    int qw_ = 0, qh_ = 0;  // quarter-res

    GpuColorTexture scene_color_;  // RGBA16F full-res  — scene renders here
    GpuColorTexture bloom_src_;    // RGBA16F qtr-res   — threshold downsample
    GpuColorTexture bloom_temp_;   // RGBA16F qtr-res   — H blur intermediate
    GpuColorTexture bloom_out_;    // RGBA16F qtr-res   — final V blur

    GpuSampler scene_sampler_;  // LINEAR CLAMP for scene reads
    GpuSampler bloom_sampler_;  // LINEAR CLAMP for bloom reads

    GpuPipeline downsample_pipe_;  // shaders/bloom_downsample.frag
    GpuPipeline blur_h_pipe_;      // shaders/bloom_blur_h.frag
    GpuPipeline blur_v_pipe_;      // shaders/bloom_blur_v.frag
    GpuPipeline composite_pipe_;   // shaders/bloom_composite.frag → swapchain
};

} // namespace md

// ── Bloom UBOs (std140 — must match shader declarations) ──────────────────────
struct BloomDownsampleUBO {
    float pixel_w;
    float pixel_h;
    float threshold;
    float _pad;
};
static_assert(sizeof(BloomDownsampleUBO) == 16);

struct BloomBlurUBO {
    float pixel_w;
    float pixel_h;
    float _pad[2];
};
static_assert(sizeof(BloomBlurUBO) == 16);

// VBfA-R5: extended to include colour grade polynomial + gamma (80B total).
struct BloomCompositeUBO {
    float bloom_intensity;  //  0
    float exposure;         //  4
    float gamma;            //  8
    float _pad;             // 12
    float eq[4][4];         // 16..79 (4 × vec4)
};
static_assert(sizeof(BloomCompositeUBO) == 80);

#endif // MD_SDL_GPU

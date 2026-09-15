#pragma once
#ifdef MD_SDL_GPU
#include <SDL3/SDL_gpu.h>
#include <monkey_dust/render/gpu_pipeline.h>
#include <monkey_dust/render/gpu_sampler_texture.h>
// ── MotionBlurSystem (VBfA-R8) ────────────────────────────────────────────────
// 2-pass screen-space motion blur.
//   Pass 1 (PrepPass)  — reconstruct world pos from depth, reproject with
//                        previous VP, pack velocity into RGBA8 motion_rt_.
//   Pass 2 (ApplyPass) — 8-tap accumulation along velocity vector (VBfA-style).
//
// Source: VBfA PostProcessor.fx  MotionBlur(), knMotionBlurSamples=8.
//
// Frame order (after geometry + post-process, before present):
//   1. MotionBlurSystem::PrepPass(cmd, hw_depth, sampler, inv_vp16, prev_vp16)
//   2. MotionBlurSystem::ApplyPass(cmd, hdr_color_tex, swapchain_tex, sw, sh)
//   3. save current VP as prev_vp for next frame
//
// Gate: IsEnabled() — reads "motion_blur" from data/render_settings.json.
// Performance: half-res prep + full-res 8-tap blur ≈ 2ms on Intel HD 520 @ 720p.

namespace md {

class MotionBlurSystem {
public:
    static MotionBlurSystem& Get();

    void Init(md::GpuDeviceHandle dev, int full_w, int full_h,
              float near_z = 0.1f, float far_z = 500.0f);
    void Shutdown();

    // Pass 1: depth + VP matrices → RGBA8 motion texture (half-res).
    // inv_vp16:  current frame inverse(view*proj), column-major float[16]
    // prev_vp16: previous frame view*proj,         column-major float[16]
    // cmd must NOT be inside an active render pass.
    void PrepPass(md::GpuCommandBufferHandle cmd,
                  md::GpuTextureHandle       hw_depth,
                  SDL_GPUSampler*       hw_sampler,
                  const float*          inv_vp16,
                  const float*          prev_vp16);

    // Pass 2: 8-tap blur of hdr_color along motion vectors.
    // Writes result directly to swapchain_tex (or any RGBA target).
    // cmd must NOT be inside an active render pass.
    void ApplyPass(md::GpuCommandBufferHandle cmd,
                   md::GpuTextureHandle       hdr_color_tex,
                   SDL_GPUSampler*       linear_sampler,
                   md::GpuTextureHandle       swapchain_tex,
                   int sw, int sh);

    md::GpuTextureHandle MotionTex()     const { return motion_rt_.SDLTexture(); }
    // Owned scene color RT (RGBA16F full-res).
    // Non-null only when motion blur is enabled — use as render target for the
    // scene pass when BloomSystem is disabled, then pass to ApplyPass.
    md::GpuTextureHandle SceneColorTex() const { return scene_color_.SDLTexture(); }
    bool IsEnabled()                const { return enabled_; }

    // Movement scale passed to the blur shader (default = {0.5,0.5}).
    // Lower = less blur; raise for fast-paced gameplay.
    float movement_scale_x = 0.5f;
    float movement_scale_y = 0.5f;

    float near_z = 0.1f;
    float far_z  = 500.0f;

private:
    MotionBlurSystem() = default;

    bool           enabled_    = false;
    md::GpuDeviceHandle dev_        = nullptr;

    GpuPipeline    prep_pipeline_;    // motion_prep.frag
    GpuPipeline    blur_pipeline_;    // motion_blur.frag

    GpuColorTexture motion_rt_;    // RGBA8_UNORM half-res
    GpuColorTexture scene_color_;  // RGBA16F full-res (own scene RT, no bloom)
    GpuSampler      point_samp_;   // NEAREST for motion read
    GpuSampler      linear_samp_;  // LINEAR for scene color read

    int half_w_ = 0, half_h_ = 0;
    int full_w_ = 0, full_h_ = 0;
};

} // namespace md

// ── MotionPrepUBO (std140, 144B — must match motion_prep.frag) ────────────────
struct MotionPrepUBO {
    float inv_view_proj[16];  //   0: current inv(V*P)  column-major
    float prev_view_proj[16]; //  64: previous V*P      column-major
    float near_z;             // 128
    float far_z;              // 132
    float _pad[2];            // 136
};
static_assert(sizeof(MotionPrepUBO) == 144);

// ── MotionBlurUBO (std140, 16B — must match motion_blur.frag) ─────────────────
struct MotionBlurUBO {
    float movement_scale_x;
    float movement_scale_y;
    float _pad[2];
};
static_assert(sizeof(MotionBlurUBO) == 16);

#endif // MD_SDL_GPU

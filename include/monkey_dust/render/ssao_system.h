#pragma once
#ifdef MD_SDL_GPU
#include <SDL3/SDL_gpu.h>
#include <monkey_dust/render/gpu_pipeline.h>
// ── SSAOSystem (VBfA-R1 + R2) ─────────────────────────────────────────────────
// Screen-Space Ambient Occlusion — VBfA-inspired approach.
//
// VBfA-R1 PrepPass: fragment-shader depth linearization.
//   Reads HW depth (D24) → writes R32F linear view-space depth at half-res.
//   No GBuffer RT1 needed — normals reconstructed from depth gradient in R2.
//
// VBfA-R2 MainPass (upcoming): 8-sample Poisson AO + bilateral blur.
//   Reads linear_depth → writes ssao_raw RGBA8 (R=AO, GB=packed edge flags).
//
// Frame order:
//   1. Geometry → depth buffer filled
//   2. SSAOSystem::PrepPass(cmd, hw_depth, hw_sampler)  ← VBfA-R1
//   3. SSAOSystem::MainPass(cmd)                        ← VBfA-R2 (upcoming)
//   4. SSAOSystem::BlurPass(cmd)                        ← VBfA-R2
//   5. Deferred lighting reads linear_depth / ssao_blurred
//
// Gate: IsEnabled() — true when RenderTierSystem::HasSSAO().

namespace md {

class SSAOSystem {
public:
    static SSAOSystem& Get();

    // Allocate textures + pipelines.  No-op if !HasSSAO().
    // near_z / far_z: camera planes for depth linearization.
    void Init(md::GpuDeviceHandle dev, int full_w, int full_h,
              float near_z = 0.1f, float far_z = 500.0f);
    void Shutdown();

    // VBfA-R1 Prep0: linearize HW depth → linear_depth R32F (half-res).
    void PrepPass(md::GpuCommandBufferHandle cmd,
                  md::GpuTextureHandle       hw_depth,
                  SDL_GPUSampler*       hw_sampler);

    // VBfA 6-pass Prep1: Sobel 3×3 normals from linear_depth → view_normals_ RGBA8.
    // Call after PrepPass, before MainPass.
    void Prep1Pass(md::GpuCommandBufferHandle cmd,
                   float inv_proj_x = 1.f, float inv_proj_y = 1.f);

    // VBfA-R2: full AO computation. Now reads view_normals_ from Prep1.
    // MainPass: (linear_depth + view_normals) → ssao_raw RGBA8.
    // BlurPass: ssao_raw → blur_temp_ (H) → ssao_blurred_ (V).
    // ApplyPass: draw fullscreen multiply-blend onto supplied swapchain texture.
    // inv_proj_x = 1/proj[0][0], inv_proj_y = 1/proj[1][1] from camera projection.
    void MainPass(md::GpuCommandBufferHandle cmd,
                  float inv_proj_x = 1.f, float inv_proj_y = 1.f);
    void BlurPass(md::GpuCommandBufferHandle cmd);
    void ApplyPass(md::GpuCommandBufferHandle cmd, md::GpuTextureHandle swapchain_tex,
                   int sw, int sh);

    // Textures
    md::GpuTextureHandle LinearDepthTex()  const { return linear_depth_; }
    md::GpuTextureHandle SSAORawTex()      const { return ssao_raw_;     }
    md::GpuTextureHandle SSAOBlurredTex()  const { return ssao_blurred_; }

    // Samplers
    SDL_GPUSampler* LinearSampler()   const { return linear_sampler_; }
    SDL_GPUSampler* PointSampler()    const { return point_sampler_;  }

    bool IsEnabled() const { return enabled_; }
    // КРОК 2 (2026-09-17, docs/DAGOR_IMPLEMENTATION_PROMPT.md): runtime
    // override for the md.set_quality_tier("ssao", 0|1|2) Lua hook --
    // separate from Init()'s own RenderTierSystem::HasSSAO() gate (tier
    // < Deferred_Med means Init() never allocated the pipelines/textures
    // this class needs, so forcing enabled_=true on such hardware would
    // just make PrepPass/MainPass/etc. silently no-op on their own null
    // checks, not crash -- but it also wouldn't actually turn SSAO on,
    // so callers on low-tier hardware won't see an effect here; this is
    // for tier>=Deferred_Med hardware where Init() already succeeded and
    // the user/quality-setting wants to toggle it off/on live).
    void SetEnabled(bool on) { enabled_ = on; }
    int  HalfW()     const { return half_w_; }
    int  HalfH()     const { return half_h_; }

    float near_z = 0.1f;
    float far_z  = 500.0f;

private:
    SSAOSystem() = default;

    bool              enabled_        = false;
    md::GpuDeviceHandle    dev_            = nullptr;

    // VBfA-R1: linear depth prep
    GpuPipeline       prep_pipeline_;            // fragment-shader depth linearize
    md::GpuTextureHandle   linear_depth_  = nullptr;  // R32F half-res
    SDL_GPUSampler*   linear_sampler_= nullptr;  // BILINEAR for final reads
    SDL_GPUSampler*   point_sampler_ = nullptr;  // NEAREST for bilateral blur

    // VBfA 6-pass: Prep1 — Sobel normals
    GpuPipeline       prep1_pipeline_;           // linear_depth→view_normals (ssao_prep1.frag)
    md::GpuTextureHandle   view_normals_  = nullptr;  // RGBA8 half-res (xyz=view normals packed)

    // VBfA-R2: AO textures + pipelines
    GpuPipeline       main_pipeline_;            // (depth+normals)→AO (ssao_main.frag)
    GpuPipeline       blur_h_pipeline_;          // bilateral H (ssao_blur_h.frag)
    GpuPipeline       blur_v_pipeline_;          // bilateral V (ssao_blur_v.frag)
    GpuPipeline       apply_pipeline_;           // multiply onto swapchain (ssao_apply.frag)
    md::GpuTextureHandle   ssao_raw_      = nullptr;  // RGBA8 half-res (R=AO, GB=packed edges)
    md::GpuTextureHandle   blur_temp_     = nullptr;  // R8   half-res (H blur intermediate)
    md::GpuTextureHandle   ssao_blurred_  = nullptr;  // R8   half-res (final AO)

    int half_w_ = 0, half_h_ = 0;
    int full_w_ = 0, full_h_ = 0;
};

} // namespace md

// ── SSAOPrepUBO (std140, 16B — must match ssao_prep.frag) ──────────────────────
struct SSAOPrepUBO {
    float near_z;
    float far_z;
    float _pad[2];
};
static_assert(sizeof(SSAOPrepUBO) == 16);

// ── SSAOPrep1UBO (std140, 16B — matches ssao_prep1.frag) ──────────────────────
struct SSAOPrep1UBO {
    float inv_proj_x;
    float inv_proj_y;
    float pixel_w;
    float pixel_h;
};
static_assert(sizeof(SSAOPrep1UBO) == 16);

// ── SSAOMainUBO (std140, 48B — must match ssao_main.frag) ──────────────────────
// PERF-17: AI.exe (Alien Isolation) RE found 4 SSAO params vs our 2.
//   occlusion_ao_infl    → ao_influence:       how much AO darkens final colour
//   specular_occlusion   → specular_occlusion: reduce specular in occluded areas
// horizon_angle_threshold/shadow_power: cherry-picked from AMD FidelityFX CACAO's
// tunable set (RENDER_VS_GRANITE_DEEPSEEK_RESEARCH.md, postprocess topic) --
// NOT the CACAO library itself, just its two most impactful scalar knobs
// grafted onto our existing 8-sample Poisson kernel.
struct SSAOMainUBO {
    float inv_proj_x;        // 1/proj[0][0] = tan(fovy/2)*aspect
    float inv_proj_y;        // 1/proj[1][1] = tan(fovy/2)
    float pixel_w;           // 1.0/half_w
    float pixel_h;           // 1.0/half_h
    float kernel_scale;      // 0.5
    float bias;              // 0.03
    float intensity;         // 1.2
    float fade_scale;        // 1.0/80.0
    float ao_influence;      // 1.0  — global AO→colour weight (AI.exe: occlusion_ao_infl)
    float specular_occlusion;// 0.3  — AO reduces specular (AI.exe: specular_occlusion)
    float horizon_angle_threshold; // 0.06 (CACAO default) — reject near-grazing-angle occlusion
    float shadow_power;      // 1.0 (neutral) — contrast curve exponent on final AO
};
static_assert(sizeof(SSAOMainUBO) == 48);

// ── SSAOBlurUBO (std140, 16B — matches ssao_blur_h.frag / ssao_blur_v.frag) ────
struct SSAOBlurUBO {
    float pixel_w;
    float pixel_h;
    float bilateral_sigma_sq; // 5.0 (CACAO default) — Gaussian falloff on AO-value
                              // difference, multiplied into the existing binary
                              // edge weight (softens blur inside non-edge regions
                              // without touching edge-preservation behavior)
    float _pad;
};
static_assert(sizeof(SSAOBlurUBO) == 16);

#endif // MD_SDL_GPU

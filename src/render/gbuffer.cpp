#ifdef MD_SDL_GPU
#include <monkey_dust/render/gbuffer.h>
#include <monkey_dust/render/render_tier.h>
#include <monkey_dust/render/gpu_pipeline.h>
#include <monkey_dust/render/gpu_hal_free_functions.h>
#include <monkey_dust/platform/md_log.h>
// ── GBuffer ───────────────────────────────────────────────────────────────────

void GBuffer::Init(md::GpuDeviceHandle dev, int w, int h) {
    dev_ = dev; w_ = w; h_ = h;

    SDL_GPUTextureCreateInfo ti = {};
    ti.type              = SDL_GPU_TEXTURETYPE_2D;
    ti.width             = static_cast<Uint32>(w);
    ti.height            = static_cast<Uint32>(h);
    ti.layer_count_or_depth = 1;
    ti.num_levels        = 1;

    // RT0: albedo(RGB) + roughness(A) — RGBA8 unorm
    ti.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    ti.usage  = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
    rt0_ = GpuCreateTexture(dev, &ti);

    // RT1: oct-normal(RG) + metallic(B) + flags(A) — R16G16B16A16 SNORM (VBfA packssdw).
    // SNORM stores normals in [-1,1] directly — no UNORM remap needed.
    // 2× precision vs RGBA8 UNORM for oct-normals; metallic/flags [0,1] map to positive half.
    ti.format = SDL_GPU_TEXTUREFORMAT_R16G16B16A16_SNORM;
    ti.usage  = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
    rt1_ = GpuCreateTexture(dev, &ti);

    // D32_FLOAT_S8_UINT: Intel Gen9 cannot sample D24_UNORM_S8_UINT (support check was
    // only for DEPTH_STENCIL_TARGET, missing SAMPLER — always use D32 for sampler safety).
    ti.format = SDL_GPU_TEXTUREFORMAT_D32_FLOAT_S8_UINT;
    ti.usage  = SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
    depth_ = SDL_CreateGPUTexture(dev, &ti);

    // Shared sampler: NEAREST + CLAMP_TO_EDGE (GBuffer reads must be pixel-precise)
    SDL_GPUSamplerCreateInfo si = {};
    si.min_filter     = SDL_GPU_FILTER_NEAREST;
    si.mag_filter     = SDL_GPU_FILTER_NEAREST;
    si.mipmap_mode    = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
    si.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    si.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    si.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    sampler_ = GpuCreateSampler(dev, &si);

    if (!rt0_ || !rt1_ || !depth_ || !sampler_) {
        MD_LOG(MD_LOG_WARNING, "GBuffer: texture creation failed (%dx%d)", w, h);
        return;
    }
    // RT0=4B + RT1=8B (R16G16B16A16_SNORM) + Depth=4B = 16 bytes/pixel
    MD_LOG(MD_LOG_INFO, "GBuffer: %dx%d RT0(RGBA8)+RT1(SNORM16)+Depth — 16 bytes/pixel = %.1f MB",
           w, h, static_cast<float>(w * h * 16) / (1024.f * 1024.f));
}

SDL_GPURenderPass* GBuffer::Begin(md::GpuCommandBufferHandle cmd) {
    // RT0=albedo+roughness clears to {0,0,0,1} (fully rough), RT1=oct-normal
    // (SNORM, OctEncode(0,0,1)=(0,0))+metallic+flags clears to {0,0,0,0} --
    // genuinely different per-target, hence clear_color_mrt1 (M1 §3 item 12).
    GpuCommandBuffer::ColorPassDesc cpd;
    cpd.cmd                 = cmd;
    cpd.color_tex[0]        = rt0_;
    cpd.color_tex[1]        = rt1_;
    cpd.num_color_targets   = 2;
    cpd.depth_tex           = depth_;
    cpd.clear_color[0]      = 0.f; cpd.clear_color[1]      = 0.f;
    cpd.clear_color[2]      = 0.f; cpd.clear_color[3]      = 1.f;
    cpd.clear_color_mrt1[0] = 0.f; cpd.clear_color_mrt1[1] = 0.f;
    cpd.clear_color_mrt1[2] = 0.f; cpd.clear_color_mrt1[3] = 0.f;
    cpd.clear_depth         = 1.f;
    cpd.load_color          = false; // CLEAR
    cpd.load_depth          = false; // CLEAR
    cb_.BeginColorPass(cpd);
    return cb_.SDLPass();
}

void GBuffer::End() {
    cb_.EndPass();
}

void GBuffer::Shutdown() {
    if (!dev_) return;
    cb_.EndPass();
    if (sampler_) { GpuReleaseSampler(dev_, sampler_); sampler_ = nullptr; }
    if (rt0_)     { GpuReleaseTexture(dev_, rt0_);     rt0_     = nullptr; }
    if (rt1_)     { GpuReleaseTexture(dev_, rt1_);     rt1_     = nullptr; }
    if (depth_)   { SDL_ReleaseGPUTexture(dev_, depth_);   depth_   = nullptr; }
    dev_ = nullptr;
}

// ── GBufferSystem ─────────────────────────────────────────────────────────────

namespace md {

GBufferSystem& GBufferSystem::Get() {
    static GBufferSystem inst;
    return inst;
}

void GBufferSystem::Init(md::GpuDeviceHandle dev, int w, int h) {
    enabled_ = RenderTierSystem::Get().IsDeferred();
    if (!enabled_) {
        MD_LOG(MD_LOG_INFO, "GBufferSystem: Forward tier — GBuffer disabled");
        return;
    }
    gbuf_.Init(dev, w, h);
}

void GBufferSystem::Shutdown() {
    if (enabled_) gbuf_.Shutdown();
    enabled_ = false;
}

} // namespace md

#endif // MD_SDL_GPU

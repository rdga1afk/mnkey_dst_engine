#ifdef MD_SDL_GPU
#include <monkey_dust/render/deferred_lighting.h>
#include <monkey_dust/render/evsm_shadow.h>
#include <monkey_dust/render/shadow_system.h>
#include <monkey_dust/render/render_tier.h>
#include <monkey_dust/render/gpu_pipeline.h>
#include <monkey_dust/render/gpu_sampler_texture.h>
#include <monkey_dust/render/gpu_device.h>
#include <monkey_dust/platform/md_log.h>
#include <cstring>

// ── DeferredAmbientUBO — must match std140 layout in deferred_lighting.frag ──
// std140 offsets:
//   0   sun_dir           vec4   16B
//  16   sun_color         vec4   16B
//  32   ambient_color     vec4   16B
//  48   emissive_scale    float   4B
//  52   shadow_dist_scalar float  4B
//  56   shadow_height_scalar float 4B
//  60   shadow_height_offset float 4B
//  64   inv_view_proj     mat4   64B  (aligned 16B ✓)
// 128   light_vp[0]       mat4   64B
// 192   light_vp[1]       mat4   64B
// 256   light_vp[2]       mat4   64B
// 320   light_vp[3]       mat4   64B  (VBfA-OPT-2: 4th cascade)
// 384   cascade_splits    vec4   16B
// 400   evsm_warp_c       float   4B
// 404   _pad[3]           float  12B
// 416   total
struct alignas(16) DeferredAmbientUBO {
    float sun_dir[4];            //   0
    float sun_color[4];          //  16
    float ambient_color[4];      //  32
    float emissive_scale;        //  48
    float shadow_dist_scalar;    //  52
    float shadow_height_scalar;  //  56
    float shadow_height_offset;  //  60
    float inv_view_proj[16];     //  64
    float light_vp[4][16];       // 128
    float cascade_splits[4];     // 384
    float evsm_warp_c;           // 400
    float _pad[3];               // 404
};
static_assert(sizeof(DeferredAmbientUBO) == 416,
              "DeferredAmbientUBO size must match deferred_lighting.frag std140");

namespace md {

DeferredLightingSystem& DeferredLightingSystem::Get() {
    static DeferredLightingSystem inst;
    return inst;
}

void DeferredLightingSystem::Init(md::GpuDeviceHandle dev, int w, int h) {
    if (!RenderTierSystem::Get().IsDeferred()) {
        MD_LOG(MD_LOG_INFO, "DeferredLightingSystem: Forward tier — disabled");
        return;
    }
    dev_ = dev; w_ = w; h_ = h;

    // HDR color target (RGBA16F — enough range before tonemapping)
    const SDL_GPUTextureUsageFlags rt_usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET
                                             | SDL_GPU_TEXTUREUSAGE_SAMPLER;
    if (!hdr_color_.Init(w, h, SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT, rt_usage)) {
        MD_LOG(MD_LOG_WARNING, "DeferredLightingSystem: failed to create HDR texture");
        return;
    }

    // NEAREST + CLAMP for GBuffer / depth reads (no interpolation on G-buffer)
    GpuSamplerDesc nearest_sdesc;
    nearest_sdesc.min_filter = GpuSamplerDesc::Filter::NEAREST;
    nearest_sdesc.mag_filter = GpuSamplerDesc::Filter::NEAREST;
    nearest_sdesc.wrap_s     = GpuSamplerDesc::Wrap::CLAMP_TO_EDGE;
    nearest_sdesc.wrap_t     = GpuSamplerDesc::Wrap::CLAMP_TO_EDGE;
    nearest_sdesc.gen_mipmap = false;
    sampler_nearest_.Init(nearest_sdesc);

    // BILINEAR + CLAMP for EVSM moment maps (bilinear improves soft shadow quality)
    GpuSamplerDesc linear_sdesc = nearest_sdesc;
    linear_sdesc.min_filter = GpuSamplerDesc::Filter::LINEAR;
    linear_sdesc.mag_filter = GpuSamplerDesc::Filter::LINEAR;
    sampler_linear_.Init(linear_sdesc);

    // Fullscreen triangle pipeline
    // frag samplers → set=2 binding 0..6: RT0, RT1, Depth, EVSM0-3
    // frag UBO      → set=3 binding=0: DeferredAmbientUBO (416B)
    GpuPipeline::Desc d;
    d.vert_path           = "shaders/deferred_lighting.vert";
    d.frag_path           = "shaders/deferred_lighting.frag";
    d.layout.count        = 0;
    d.layout.stride       = 0;
    d.raster.blend_enable = false;
    d.raster.depth_test   = false;
    d.raster.depth_write  = false;
    d.raster.cull_back    = false;
    d.vert_uniform_bufs   = 0;
    d.frag_uniform_bufs   = 1;
    d.frag_samplers       = 7;   // slots 0-2: RT0/RT1/Depth; slots 3-6: EVSM×4
    d.has_depth_target    = false;
    d.depth_only          = false;

    if (!pipeline_.Create(d)) {
        MD_LOG(MD_LOG_WARNING, "DeferredLightingSystem: pipeline create failed");
        hdr_color_.Shutdown();
        return;
    }

    MD_LOG(MD_LOG_INFO, "DeferredLightingSystem: ready (%dx%d RGBA16F, EVSM shadows)", w, h);
}

void DeferredLightingSystem::DrawAmbientPass(md::GpuCommandBufferHandle cmd,
                                              const GBuffer& gbuf,
                                              md::GpuTextureHandle depth_tex) {
    if (!hdr_color_.SDLTexture()) return;

    GpuCommandBuffer cb;
    GpuCommandBuffer::ColorPassDesc cpd;
    cpd.cmd            = cmd;
    cpd.color_tex[0]      = hdr_color_.SDLTexture();
    cpd.clear_color[0] = 0.f; cpd.clear_color[1] = 0.f;
    cpd.clear_color[2] = 0.f; cpd.clear_color[3] = 1.f;
    cpd.load_color     = false; // CLEAR, matches original exactly
    cb.BeginColorPass(cpd);
    if (!cb.SDLPass()) {
        MD_LOG(MD_LOG_WARNING, "DeferredLightingSystem: BeginGPURenderPass failed");
        return;
    }

    cb.BindPipeline(&pipeline_);

    // ── slots 0-2: GBuffer RT0, RT1, Depth → SPIR-V set=2 binding=0/1/2 ───────
    {
        SDL_GPUTextureSamplerBinding b[3] = {};
        b[0] = { gbuf.RT0(), sampler_nearest_.SDLSampler() };
        b[1] = { gbuf.RT1(), sampler_nearest_.SDLSampler() };
        // Depth: use the scene depth texture if provided, otherwise a dummy.
        // Shader guards against out-of-bounds reconstruction (depth == 0 or 1 → skip).
        b[2] = { depth_tex ? depth_tex : gbuf.RT0(),  // fallback: no shadow if no depth
                 sampler_nearest_.SDLSampler() };
        cb.BindFragmentSamplers(0, b, 3);
    }

    // ── slots 3-6: EVSM moment maps → SPIR-V set=2 binding=3/4/5/6 ─────────
    const md::EvsmShadow& evsm = md::EvsmShadow::Get();
    const bool evsm_ready = evsm.IsReady() && depth_tex;
    {
        md::GpuTextureHandle fallback = gbuf.RT0();  // safe dummy (never sampled if !evsm_ready)
        SDL_GPUTextureSamplerBinding b[4] = {};
        b[0] = { evsm_ready ? evsm.MomentTex(0) : fallback, sampler_linear_.SDLSampler() };
        b[1] = { evsm_ready ? evsm.MomentTex(1) : fallback, sampler_linear_.SDLSampler() };
        b[2] = { evsm_ready ? evsm.MomentTex(2) : fallback, sampler_linear_.SDLSampler() };
        b[3] = { evsm_ready ? evsm.MomentTex(3) : fallback, sampler_linear_.SDLSampler() };
        cb.BindFragmentSamplers(3, b, 4);   // slots 3,4,5,6
    }

    // ── frag UBO slot=0 → SPIR-V set=3 binding=0: DeferredAmbientUBO ────────
    {
        DeferredAmbientUBO ubo = {};

        // Lighting
        ubo.sun_dir[0]  = sun_dir[0];  ubo.sun_dir[1]  = sun_dir[1];
        ubo.sun_dir[2]  = sun_dir[2];  ubo.sun_dir[3]  = 0.0f;
        ubo.sun_color[0] = sun_color[0]; ubo.sun_color[1] = sun_color[1];
        ubo.sun_color[2] = sun_color[2]; ubo.sun_color[3] = 0.0f;
        ubo.ambient_color[0] = ambient_color[0]; ubo.ambient_color[1] = ambient_color[1];
        ubo.ambient_color[2] = ambient_color[2]; ubo.ambient_color[3] = 0.0f;
        ubo.emissive_scale       = emissive_scale;
        ubo.shadow_dist_scalar   = 1.f / 120.f;
        ubo.shadow_height_scalar = 0.f;
        ubo.shadow_height_offset = 1.f;

        // EVSM shadow data
        const ShadowSystem& ss = ShadowSystem::Get();

        // inv_view_proj: reconstructs world position from depth buffer + NDC.
        // Computed from the camera view-projection matrix stored in ShadowSystem
        // (ShadowSystem already has the current camera VP via Update()).
        // We store inv_vp separately: caller sets it via SetInvViewProj().
        memcpy(ubo.inv_view_proj, inv_view_proj_, 64);

        // Per-cascade light-space VP matrices (from ShadowSystem::lightViewProj[])
        for (int k = 0; k < ShadowSystem::NUM_CASCADES; ++k)
            memcpy(ubo.light_vp[k], mat4_ptr(ss.lightViewProj[k]), 64);

        // Cascade far planes (view-space Z) for cascade selection in shader.
        ubo.cascade_splits[0] = ss.cascade_splits[0];  // 20.0m
        ubo.cascade_splits[1] = ss.cascade_splits[1];  // 60.0m
        ubo.cascade_splits[2] = ss.cascade_splits[2];  // 150.0m
        ubo.cascade_splits[3] = ss.cascade_splits[3];  // 350.0m (VBfA-OPT-2)

        ubo.evsm_warp_c = evsm_ready ? evsm.WarpC() : 40.f;

        cb.PushFragmentUniforms(0, &ubo, sizeof(ubo));
    }

    cb.Draw(3);
    cb.EndPass();
}

void DeferredLightingSystem::Shutdown() {
    if (!dev_) return;
    pipeline_.Destroy();
    hdr_color_.Shutdown();
    sampler_nearest_.Shutdown();
    sampler_linear_.Shutdown();
    dev_ = nullptr;
}

} // namespace md
#endif // MD_SDL_GPU

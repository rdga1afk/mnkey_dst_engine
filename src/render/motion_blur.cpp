#ifdef MD_SDL_GPU
#include <monkey_dust/render/motion_blur.h>
#include <monkey_dust/render/gpu_pipeline.h>
#include <monkey_dust/render/gpu_sampler_texture.h>
#include <monkey_dust/render/gpu_pass_view.h>
#include <monkey_dust/render/gpu_hal_free_functions.h>
#include <monkey_dust/platform/md_log.h>
#include <monkey_dust/platform/md_fs.h>
#include <cstring>

namespace md {

MotionBlurSystem& MotionBlurSystem::Get() {
    static MotionBlurSystem inst;
    return inst;
}

// ── Init ─────────────────────────────────────────────────────────────────────

void MotionBlurSystem::Init(md::GpuDeviceHandle dev, int full_w, int full_h,
                             float nz, float fz) {
    // Gate: check render_settings.json for "motion_blur": true
    {
        uint32_t sz = 0;
        char* buf = md::fs_read_alloc("data/render_settings.json", &sz);
        bool want = false;
        if (buf) {
            const char* p = strstr(buf, "\"motion_blur\"");
            if (p) {
                p = strchr(p + 13, ':');
                if (p) {
                    while (*p == ':' || *p == ' ' || *p == '\t') ++p;
                    want = (strncmp(p, "true", 4) == 0);
                }
            }
            md::fs_free(buf);
        }
        if (!want) {
            MD_LOG(MD_LOG_INFO, "MotionBlurSystem: disabled (render_settings.json motion_blur=false)");
            return;
        }
    }

    dev_    = dev;
    full_w_ = full_w;
    full_h_ = full_h;
    half_w_ = full_w / 2;
    half_h_ = full_h / 2;
    near_z  = nz;
    far_z   = fz;

    const SDL_GPUTextureUsageFlags rt_usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET
                                             | SDL_GPU_TEXTUREUSAGE_SAMPLER;

    // ── RGBA8 motion render target (half-res) ─────────────────────────────────
    if (!motion_rt_.Init(half_w_, half_h_, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, rt_usage)) {
        MD_LOG(MD_LOG_WARNING, "MotionBlurSystem: motion_rt create failed: %s", SDL_GetError());
        return;
    }

    // ── NEAREST sampler for motion texture read ───────────────────────────────
    {
        GpuSamplerDesc sdesc;
        sdesc.min_filter = GpuSamplerDesc::Filter::NEAREST;
        sdesc.mag_filter = GpuSamplerDesc::Filter::NEAREST;
        sdesc.wrap_s     = GpuSamplerDesc::Wrap::CLAMP_TO_EDGE;
        sdesc.wrap_t     = GpuSamplerDesc::Wrap::CLAMP_TO_EDGE;
        sdesc.gen_mipmap = false;
        if (!point_samp_.Init(sdesc)) {
            MD_LOG(MD_LOG_WARNING, "MotionBlurSystem: point_samp create failed");
            return;
        }
    }

    // ── LINEAR sampler for scene color read ──────────────────────────────────
    {
        GpuSamplerDesc sdesc;
        sdesc.min_filter = GpuSamplerDesc::Filter::LINEAR;
        sdesc.mag_filter = GpuSamplerDesc::Filter::LINEAR;
        sdesc.wrap_s     = GpuSamplerDesc::Wrap::CLAMP_TO_EDGE;
        sdesc.wrap_t     = GpuSamplerDesc::Wrap::CLAMP_TO_EDGE;
        sdesc.gen_mipmap = false;
        if (!linear_samp_.Init(sdesc)) {
            MD_LOG(MD_LOG_WARNING, "MotionBlurSystem: linear_samp create failed");
            return;
        }
    }

    // ── RGBA16F scene color RT (used when bloom is not active) ────────────────
    if (!scene_color_.Init(full_w_, full_h_, SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT, rt_usage)) {
        MD_LOG(MD_LOG_WARNING, "MotionBlurSystem: scene_color create failed: %s", SDL_GetError());
        return;
    }

    // ── Prep pipeline: depth → RGBA8 motion (half-res) ───────────────────────
    {
        GpuPipeline::Desc d;
        d.vert_path           = "shaders/deferred_lighting.vert";
        d.frag_path           = "shaders/motion_prep.frag";
        d.layout.count        = 0;
        d.layout.stride       = 0;
        d.raster.blend_enable = false;
        d.raster.depth_test   = false;
        d.raster.depth_write  = false;
        d.raster.cull_back    = false;
        d.frag_samplers       = 1;   // set=0 b=0: u_depth
        d.frag_uniform_bufs   = 1;   // set=1 b=0: MotionPrepUBO
        d.has_depth_target    = false;
        d.color_format        = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
        if (!prep_pipeline_.Create(d)) {
            MD_LOG(MD_LOG_WARNING, "MotionBlurSystem: prep pipeline create failed");
            return;
        }
    }

    // ── Blur pipeline: motion + hdr_color → swapchain (full-res) ─────────────
    {
        GpuPipeline::Desc d;
        d.vert_path           = "shaders/deferred_lighting.vert";
        d.frag_path           = "shaders/motion_blur.frag";
        d.layout.count        = 0;
        d.layout.stride       = 0;
        d.raster.blend_enable = false;
        d.raster.depth_test   = false;
        d.raster.depth_write  = false;
        d.raster.cull_back    = false;
        d.frag_samplers       = 2;   // set=0 b=0: u_motion, b=1: u_color
        d.frag_uniform_bufs   = 1;   // set=1 b=0: MotionBlurUBO
        d.has_depth_target    = false;
        d.color_format        = SDL_GPU_TEXTUREFORMAT_INVALID; // swapchain format
        if (!blur_pipeline_.Create(d)) {
            MD_LOG(MD_LOG_WARNING, "MotionBlurSystem: blur pipeline create failed");
            return;
        }
    }

    enabled_ = true;
    MD_LOG(MD_LOG_INFO, "MotionBlurSystem: %dx%d motion RT, 8-tap blur ready",
           half_w_, half_h_);
}

// ── PrepPass ─────────────────────────────────────────────────────────────────

void MotionBlurSystem::PrepPass(md::GpuCommandBufferHandle cmd,
                                 md::GpuTextureHandle       hw_depth,
                                 SDL_GPUSampler*       hw_sampler,
                                 const float*          inv_vp16,
                                 const float*          prev_vp16) {
    if (!enabled_ || !motion_rt_.SDLTexture() || !prep_pipeline_.SDLPipeline()) return;

    GpuCommandBuffer cb;
    GpuCommandBuffer::ColorPassDesc cpd;
    cpd.cmd             = cmd;
    cpd.color_tex[0]       = motion_rt_.SDLTexture();
    cpd.color_dont_care = true; // matches original LOADOP_DONT_CARE
    cpd.clear_color[0] = 0.5f; cpd.clear_color[1] = 0.5f; // neutral motion (unused under DONT_CARE, kept for parity)
    cpd.clear_color[2] = 0.5f; cpd.clear_color[3] = 1.f;
    cb.BeginColorPass(cpd);
    SDL_GPURenderPass* pass = cb.SDLPass();
    if (!pass) {
        MD_LOG(MD_LOG_WARNING, "MotionBlurSystem::PrepPass: begin failed: %s", SDL_GetError());
        return;
    }

    GpuPassView pv = GpuPassView::FromRaw(pass, cmd);
    pv.BindPipeline(&prep_pipeline_);

    SDL_GPUTextureSamplerBinding sb = { hw_depth, hw_sampler };
    pv.BindFragmentSamplers(0, &sb, 1);

    MotionPrepUBO ubo;
    memcpy(ubo.inv_view_proj, inv_vp16,  64);
    memcpy(ubo.prev_view_proj, prev_vp16, 64);
    ubo.near_z  = near_z;
    ubo.far_z   = far_z;
    ubo._pad[0] = 0.f;
    ubo._pad[1] = 0.f;
    pv.PushFragmentUniforms(0, &ubo, sizeof(ubo));

    SDL_GPUViewport vp = { 0.f, 0.f, (float)half_w_, (float)half_h_, 0.f, 1.f };
    GpuSetViewport(pass, vp);
    pv.Draw(3, 1, 0, 0);

    cb.EndPass();
}

// ── ApplyPass ─────────────────────────────────────────────────────────────────

void MotionBlurSystem::ApplyPass(md::GpuCommandBufferHandle cmd,
                                  md::GpuTextureHandle       hdr_color_tex,
                                  SDL_GPUSampler*       linear_sampler,
                                  md::GpuTextureHandle       swapchain_tex,
                                  int sw, int sh) {
    if (!enabled_ || !motion_rt_.SDLTexture() || !blur_pipeline_.SDLPipeline()) return;
    if (!hdr_color_tex || !swapchain_tex) return;

    // Write directly to swapchain (DONT_CARE = we replace the entire image)
    GpuCommandBuffer cb;
    GpuCommandBuffer::ColorPassDesc cpd;
    cpd.cmd             = cmd;
    cpd.color_tex[0]       = swapchain_tex;
    cpd.color_dont_care = true; // matches original LOADOP_DONT_CARE
    cb.BeginColorPass(cpd);
    SDL_GPURenderPass* pass = cb.SDLPass();
    if (!pass) {
        MD_LOG(MD_LOG_WARNING, "MotionBlurSystem::ApplyPass: begin failed: %s", SDL_GetError());
        return;
    }

    GpuPassView pv = GpuPassView::FromRaw(pass, cmd);
    pv.BindPipeline(&blur_pipeline_);

    SDL_GPUSampler* col_samp = linear_sampler ? linear_sampler : linear_samp_.SDLSampler();
    SDL_GPUTextureSamplerBinding sbs[2] = {
        { motion_rt_.SDLTexture(), point_samp_.SDLSampler() },  // set=0 b=0: u_motion
        { hdr_color_tex,           col_samp                },  // set=0 b=1: u_color
    };
    pv.BindFragmentSamplers(0, sbs, 2);

    MotionBlurUBO ubo = { movement_scale_x, movement_scale_y, {0.f, 0.f} };
    pv.PushFragmentUniforms(0, &ubo, sizeof(ubo));

    SDL_GPUViewport vp = { 0.f, 0.f, (float)sw, (float)sh, 0.f, 1.f };
    GpuSetViewport(pass, vp);
    pv.Draw(3, 1, 0, 0);

    cb.EndPass();
}

// ── Shutdown ─────────────────────────────────────────────────────────────────

void MotionBlurSystem::Shutdown() {
    if (!dev_) return;
    prep_pipeline_.Destroy();
    blur_pipeline_.Destroy();
    motion_rt_.Shutdown(); scene_color_.Shutdown();
    point_samp_.Shutdown(); linear_samp_.Shutdown();
    dev_     = nullptr;
    enabled_ = false;
}

} // namespace md
#endif // MD_SDL_GPU

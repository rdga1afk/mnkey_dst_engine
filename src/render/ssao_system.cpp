#ifdef MD_SDL_GPU
#include <monkey_dust/render/ssao_system.h>
#include <monkey_dust/render/render_tier.h>
#include <monkey_dust/render/gpu_hal_types.h>
#include <monkey_dust/render/gpu_pipeline.h>
#include <monkey_dust/render/gpu_pass_view.h>
#include <monkey_dust/render/gpu_hal_free_functions.h>
#include <monkey_dust/platform/md_log.h>

namespace md {

SSAOSystem& SSAOSystem::Get() {
    static SSAOSystem inst;
    return inst;
}

void SSAOSystem::Init(md::GpuDeviceHandle dev, int full_w, int full_h,
                      float nz, float fz) {
    if (!RenderTierSystem::Get().HasSSAO()) {
        MD_LOG(MD_LOG_INFO, "SSAOSystem: tier < Deferred_Med — disabled");
        return;
    }
    dev_    = dev;
    full_w_ = full_w;
    full_h_ = full_h;
    half_w_ = full_w / 2;
    half_h_ = full_h / 2;
    near_z  = nz;
    far_z   = fz;

    // ── VBfA-R1: linear depth texture (R32F, half-res) ───────────────────────
    {
        SDL_GPUTextureCreateInfo ti = {};
        ti.type                 = SDL_GPU_TEXTURETYPE_2D;
        ti.width                = (Uint32)half_w_;
        ti.height               = (Uint32)half_h_;
        ti.layer_count_or_depth = 1;
        ti.num_levels           = 1;
        ti.format               = SDL_GPU_TEXTUREFORMAT_R32_FLOAT;
        ti.usage                = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET
                                | SDL_GPU_TEXTUREUSAGE_SAMPLER;
        linear_depth_ = GpuCreateTexture(dev, &ti);
        if (!linear_depth_) {
            MD_LOG(MD_LOG_WARNING, "SSAOSystem: linear_depth create failed: %s", SDL_GetError());
            return;
        }
    }

    // BILINEAR sampler — for reading linear_depth in AO main pass
    {
        SDL_GPUSamplerCreateInfo si = {};
        si.min_filter     = SDL_GPU_FILTER_LINEAR;
        si.mag_filter     = SDL_GPU_FILTER_LINEAR;
        si.mipmap_mode    = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
        si.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
        si.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
        si.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
        linear_sampler_ = GpuCreateSampler(dev, &si);
        if (!linear_sampler_) {
            MD_LOG(MD_LOG_WARNING, "SSAOSystem: linear_sampler create failed");
            return;
        }
    }

    // NEAREST sampler — for bilateral blur (reads packed edge flags)
    {
        SDL_GPUSamplerCreateInfo si = {};
        si.min_filter     = SDL_GPU_FILTER_NEAREST;
        si.mag_filter     = SDL_GPU_FILTER_NEAREST;
        si.mipmap_mode    = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
        si.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
        si.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
        si.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
        point_sampler_ = GpuCreateSampler(dev, &si);
        if (!point_sampler_) {
            MD_LOG(MD_LOG_WARNING, "SSAOSystem: point_sampler create failed");
            return;
        }
    }

    // ── VBfA-R1: prep pipeline (fullscreen triangle, depth→R32F) ─────────────
    {
        GpuPipeline::Desc d;
        d.vert_path           = "shaders/deferred_lighting.vert";  // same fullscreen tri
        d.frag_path           = "shaders/ssao_prep.frag";
        d.layout.count        = 0;
        d.layout.stride       = 0;
        d.raster.blend_enable = false;
        d.raster.depth_test   = false;
        d.raster.depth_write  = false;
        d.raster.cull_back    = false;
        d.vert_uniform_bufs   = 0;
        d.frag_samplers       = 1;   // set=0 binding=0: u_depth
        d.frag_uniform_bufs   = 1;   // set=1 binding=0: SSAOPrepUBO
        d.has_depth_target = false;
        d.depth_only       = false;
        d.color_format     = SDL_GPU_TEXTUREFORMAT_R32_FLOAT;

        if (!prep_pipeline_.Create(d)) {
            MD_LOG(MD_LOG_WARNING, "SSAOSystem: prep pipeline create failed");
            return;
        }
    }

    // ── VBfA-R2: AO textures ─────────────────────────────────────────────────
    auto make_rt = [&](SDL_GPUTextureFormat fmt, md::GpuTextureHandle& out) {
        SDL_GPUTextureCreateInfo ti = {};
        ti.type                 = SDL_GPU_TEXTURETYPE_2D;
        ti.width                = (Uint32)half_w_;
        ti.height               = (Uint32)half_h_;
        ti.layer_count_or_depth = 1;
        ti.num_levels           = 1;
        ti.format               = fmt;
        ti.usage                = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET
                                | SDL_GPU_TEXTUREUSAGE_SAMPLER;
        out = GpuCreateTexture(dev, &ti);
        return out != nullptr;
    };

    if (!make_rt(SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, view_normals_) ||
        !make_rt(SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, ssao_raw_)    ||
        !make_rt(SDL_GPU_TEXTUREFORMAT_R8_UNORM,       blur_temp_)   ||
        !make_rt(SDL_GPU_TEXTUREFORMAT_R8_UNORM,       ssao_blurred_)) {
        MD_LOG(MD_LOG_WARNING, "SSAOSystem: R2 texture create failed: %s", SDL_GetError());
    }

    // ── VBfA-R2: AO pipelines ────────────────────────────────────────────────
    // Helper: build a fullscreen-triangle pipeline targeting a given format
    auto make_fs_pipe = [&](const char* frag, int samplers, int ubos,
                            SDL_GPUTextureFormat fmt, GpuPipeline& pipe) {
        GpuPipeline::Desc d;
        d.vert_path           = "shaders/deferred_lighting.vert";
        d.frag_path           = frag;
        d.layout.count        = 0;
        d.layout.stride       = 0;
        d.raster.blend_enable = false;
        d.raster.depth_test   = false;
        d.raster.depth_write  = false;
        d.raster.cull_back    = false;
        d.frag_samplers       = (uint32_t)samplers;
        d.frag_uniform_bufs   = (uint32_t)ubos;
        d.has_depth_target    = false;
        d.color_format        = fmt;
        if (!pipe.Create(d))
            MD_LOG(MD_LOG_WARNING, "SSAOSystem: pipeline create failed: %s", frag);
    };

    // Prep1: linear_depth → view_normals (Sobel 3×3 normals pass)
    if (view_normals_)
        make_fs_pipe("shaders/ssao_prep1.frag",  1, 1,
                     SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, prep1_pipeline_);
    // Main: (linear_depth + view_normals) → ssao_raw  (2 samplers now)
    if (ssao_raw_)
        make_fs_pipe("shaders/ssao_main.frag",   2, 1,
                     SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, main_pipeline_);
    if (blur_temp_)
        make_fs_pipe("shaders/ssao_blur_h.frag", 1, 1,
                     SDL_GPU_TEXTUREFORMAT_R8_UNORM, blur_h_pipeline_);
    if (ssao_blurred_) {
        // V blur reads 2 samplers (u_ssao_h + u_ssao_raw)
        make_fs_pipe("shaders/ssao_blur_v.frag", 2, 1,
                     SDL_GPU_TEXTUREFORMAT_R8_UNORM, blur_v_pipeline_);
    }

    // Apply: multiply-blend onto swapchain (INVALID = use swapchain format)
    {
        GpuPipeline::Desc d;
        d.vert_path           = "shaders/deferred_lighting.vert";
        d.frag_path           = "shaders/ssao_apply.frag";
        d.layout.count        = 0;
        d.layout.stride       = 0;
        d.raster.blend_enable = true;
        d.raster.src_factor   = GpuBlendFactor::SRC_ALPHA;
        d.raster.dst_factor   = GpuBlendFactor::ONE_MINUS_SRC_ALPHA;
        d.raster.depth_test   = false;
        d.raster.depth_write  = false;
        d.raster.cull_back    = false;
        d.frag_samplers       = 1;
        d.frag_uniform_bufs   = 0;
        d.has_depth_target    = false;
        d.color_format        = SDL_GPU_TEXTUREFORMAT_INVALID; // swapchain format
        if (!apply_pipeline_.Create(d))
            MD_LOG(MD_LOG_WARNING, "SSAOSystem: apply pipeline create failed");
    }

    enabled_ = true;
    MD_LOG(MD_LOG_INFO, "SSAOSystem R1+R2: %dx%d — prep+main+blur+apply ready",
           half_w_, half_h_);
}

void SSAOSystem::PrepPass(md::GpuCommandBufferHandle cmd,
                          md::GpuTextureHandle       hw_depth,
                          SDL_GPUSampler*       hw_sampler) {
    if (!enabled_ || !linear_depth_ || !prep_pipeline_.SDLPipeline()) return;

    // Render to linear_depth_ at half-res (no depth attachment)
    GpuCommandBuffer cb;
    GpuCommandBuffer::ColorPassDesc cpd;
    cpd.cmd             = cmd;
    cpd.color_tex[0]       = linear_depth_;
    cpd.color_dont_care = true; // matches original LOADOP_DONT_CARE
    cb.BeginColorPass(cpd);
    SDL_GPURenderPass* pass = cb.SDLPass();
    if (!pass) {
        MD_LOG(MD_LOG_WARNING, "SSAOSystem::PrepPass: begin failed: %s", SDL_GetError());
        return;
    }

    GpuPassView pv = GpuPassView::FromRaw(pass, cmd);
    pv.BindPipeline(&prep_pipeline_);

    // set=0 binding=0: hw depth texture
    SDL_GPUTextureSamplerBinding sb = { hw_depth, hw_sampler };
    pv.BindFragmentSamplers(0, &sb, 1);

    // set=1 binding=0: SSAOPrepUBO
    SSAOPrepUBO ubo = { near_z, far_z, {0.f, 0.f} };
    pv.PushFragmentUniforms(0, &ubo, sizeof(ubo));

    // Fullscreen triangle: 3 verts, no VBO
    SDL_GPUViewport vp = { 0.f, 0.f, (float)half_w_, (float)half_h_, 0.f, 1.f };
    GpuSetViewport(pass, vp);
    pv.Draw(3, 1, 0, 0);

    cb.EndPass();
}

// ── Helper: begin a fullscreen render pass on an RT, draw 3 verts, end ────────
static void FullscreenPass(md::GpuCommandBufferHandle cmd, GpuPipeline* pipeline,
                            md::GpuTextureHandle target, int tw, int th,
                            const SDL_GPUTextureSamplerBinding* sbs, int nsbs,
                            const void* frag_ubo, uint32_t frag_ubo_sz) {
    GpuCommandBuffer cb;
    GpuCommandBuffer::ColorPassDesc cpd;
    cpd.cmd             = cmd;
    cpd.color_tex[0]       = target;
    cpd.color_dont_care = true; // matches original LOADOP_DONT_CARE
    cb.BeginColorPass(cpd);
    SDL_GPURenderPass* pass = cb.SDLPass();
    if (!pass) return;

    GpuPassView pv = GpuPassView::FromRaw(pass, cmd);
    pv.BindPipeline(pipeline);
    if (nsbs > 0)
        pv.BindFragmentSamplers(0, sbs, (uint32_t)nsbs);
    if (frag_ubo)
        pv.PushFragmentUniforms(0, frag_ubo, frag_ubo_sz);

    SDL_GPUViewport vp = { 0.f, 0.f, (float)tw, (float)th, 0.f, 1.f };
    GpuSetViewport(pass, vp);
    pv.Draw(3, 1, 0, 0);

    cb.EndPass();
}

// ── VBfA 6-pass Prep1: Sobel 3×3 normals from linear_depth ──────────────────
void SSAOSystem::Prep1Pass(md::GpuCommandBufferHandle cmd,
                            float inv_proj_x, float inv_proj_y) {
    if (!enabled_ || !view_normals_ || !prep1_pipeline_.SDLPipeline()) return;
    if (!linear_depth_) return;

    SSAOPrep1UBO ubo = { inv_proj_x, inv_proj_y, 1.f/(float)half_w_, 1.f/(float)half_h_ };
    SDL_GPUTextureSamplerBinding sb = { linear_depth_, linear_sampler_ };
    FullscreenPass(cmd, &prep1_pipeline_,
                   view_normals_, half_w_, half_h_,
                   &sb, 1, &ubo, sizeof(ubo));
}

void SSAOSystem::MainPass(md::GpuCommandBufferHandle cmd,
                          float inv_px, float inv_py) {
    if (!enabled_ || !ssao_raw_ || !main_pipeline_.SDLPipeline()) return;
    if (!linear_depth_)  return;

    SSAOMainUBO ubo;
    ubo.inv_proj_x   = inv_px;
    ubo.inv_proj_y   = inv_py;
    ubo.pixel_w      = 1.f / (float)half_w_;
    ubo.pixel_h      = 1.f / (float)half_h_;
    ubo.kernel_scale = 0.5f;
    ubo.bias         = 0.03f;
    ubo.intensity    = 1.2f;
    ubo.fade_scale   = 1.f / 80.f;
    // These two were previously left uninitialized (POD `SSAOMainUBO ubo;`
    // with no member-init) -- fixing that latent bug while wiring the field.
    ubo.ao_influence = 1.0f;
    ubo.specular_occlusion = 0.3f;
    // CACAO-cherry-picked tunables (RENDER_VS_GRANITE_DEEPSEEK_RESEARCH.md).
    ubo.horizon_angle_threshold = 0.06f;
    ubo.shadow_power            = 1.0f; // neutral; report gave no CACAO default

    // Two samplers: b=0 = linear_depth, b=1 = view_normals (from Prep1)
    SDL_GPUTextureSamplerBinding sbs[2] = {
        { linear_depth_, linear_sampler_ },
        { view_normals_ ? view_normals_ : linear_depth_, linear_sampler_ }
    };
    int nsbs = view_normals_ ? 2 : 1;
    FullscreenPass(cmd, &main_pipeline_,
                   ssao_raw_, half_w_, half_h_,
                   sbs, nsbs, &ubo, sizeof(ubo));
}

void SSAOSystem::BlurPass(md::GpuCommandBufferHandle cmd) {
    if (!enabled_ || !blur_temp_ || !ssao_blurred_) return;
    if (!blur_h_pipeline_.SDLPipeline() || !blur_v_pipeline_.SDLPipeline()) return;

    // bilateral_sigma_sq = 5.0 (CACAO default, RENDER_VS_GRANITE_DEEPSEEK_
    // RESEARCH.md postprocess topic) -- Gaussian falloff multiplied into the
    // existing binary edge weight.
    SSAOBlurUBO ubo = { 1.f/(float)half_w_, 1.f/(float)half_h_, 5.0f, 0.f };

    // Horizontal: ssao_raw → blur_temp_
    {
        SDL_GPUTextureSamplerBinding sb = { ssao_raw_, point_sampler_ };
        FullscreenPass(cmd, &blur_h_pipeline_,
                       blur_temp_, half_w_, half_h_,
                       &sb, 1, &ubo, sizeof(ubo));
    }

    // Vertical: blur_temp_ + ssao_raw_ → ssao_blurred_
    {
        SDL_GPUTextureSamplerBinding sbs[2] = {
            { blur_temp_, point_sampler_ },
            { ssao_raw_,  point_sampler_ }
        };
        FullscreenPass(cmd, &blur_v_pipeline_,
                       ssao_blurred_, half_w_, half_h_,
                       sbs, 2, &ubo, sizeof(ubo));
    }
}

void SSAOSystem::ApplyPass(md::GpuCommandBufferHandle cmd,
                            md::GpuTextureHandle swapchain_tex, int sw, int sh) {
    if (!enabled_ || !ssao_blurred_ || !apply_pipeline_.SDLPipeline()) return;
    if (!swapchain_tex) return;

    // Open a LOAD render pass on the swapchain (don't clear the scene)
    GpuCommandBuffer cb;
    GpuCommandBuffer::ColorPassDesc cpd;
    cpd.cmd        = cmd;
    cpd.color_tex[0]  = swapchain_tex;
    cpd.load_color = true; // LOAD, preserve existing scene
    cb.BeginColorPass(cpd);
    if (!cb.SDLPass()) return;
    SDL_GPURenderPass* pass = cb.SDLPass();

    cb.BindPipeline(&apply_pipeline_);

    SDL_GPUTextureSamplerBinding sb = { ssao_blurred_, linear_sampler_ };
    cb.BindFragmentSamplers(0, &sb, 1);

    SDL_GPUViewport vp = { 0.f, 0.f, (float)sw, (float)sh, 0.f, 1.f };
    GpuSetViewport(pass, vp);
    cb.Draw(3);

    cb.EndPass();
}

void SSAOSystem::Shutdown() {
    if (!dev_) return;
    prep_pipeline_.Destroy();
    main_pipeline_.Destroy();
    blur_h_pipeline_.Destroy();
    blur_v_pipeline_.Destroy();
    apply_pipeline_.Destroy();
    auto rel_tex = [&](md::GpuTextureHandle& t){
        if (t) { GpuReleaseTexture(dev_, t); t = nullptr; }
    };
    auto rel_sam = [&](SDL_GPUSampler*& s){
        if (s) { GpuReleaseSampler(dev_, s); s = nullptr; }
    };
    rel_tex(linear_depth_);
    rel_tex(ssao_raw_);
    rel_tex(blur_temp_);
    rel_tex(ssao_blurred_);
    rel_sam(linear_sampler_);
    rel_sam(point_sampler_);
    dev_     = nullptr;
    enabled_ = false;
}

} // namespace md
#endif // MD_SDL_GPU

#include <monkey_dust/render/evsm_shadow.h>
#ifdef MD_SDL_GPU
#include <monkey_dust/render/gpu_device.h>
#include <monkey_dust/render/gpu_pipeline.h>
#include <monkey_dust/render/gpu_hal_free_functions.h>
#include <monkey_dust/platform/md_fs.h>
#include <cstdio>

namespace md {

// ── Local SPIR-V loader ───────────────────────────────────────────────────────
static SDL_GPUShader* LoadSpv(md::GpuDeviceHandle dev, const char* spv_path,
                               SDL_GPUShaderStage stage,
                               uint32_t uni = 0, uint32_t smp = 0) {
    uint32_t sz = 0;
    uint8_t* code = (uint8_t*)md::fs_read_alloc(spv_path, &sz);
    if (!code) {
        fprintf(stderr, "[EvsmShadow] SPIR-V not found: %s\n", spv_path);
        return nullptr;
    }
    SDL_GPUShaderCreateInfo info{};
    info.code                = code;
    info.code_size           = sz;
    info.entrypoint          = "main";
    info.format              = SDL_GPU_SHADERFORMAT_SPIRV;
    info.stage               = stage;
    info.num_uniform_buffers = uni;
    info.num_samplers        = smp;
    SDL_GPUShader* sh = GpuCreateShader(dev, &info);
    md::fs_free((char*)code);
    return sh;
}

// ── Init / Shutdown ───────────────────────────────────────────────────────────

bool EvsmShadow::Init(int num_cascades, int map_size, float warp_c) {
    num_cascades_ = (num_cascades > 0 && num_cascades <= NUM_CASCADES)
                    ? num_cascades : NUM_CASCADES;
    map_size_ = map_size;
    warp_c_   = warp_c;

    md::GpuDeviceHandle dev = GpuDevice::Get().SDLDevice();
    if (!dev) return false;

    // ── Moment textures: R16G16_FLOAT (universally supported as color target) ───
    // R32G32_FLOAT crashes SDL_CreateGPUGraphicsPipeline on Intel ANV (Gen9/HD 520)
    // even when SDL_GPUTextureSupportsFormat reports it as supported (ANV driver bug).
    // R16G16_FLOAT is sufficient for EVSM moments and safe on all hardware.
    const SDL_GPUTextureUsageFlags ct_usage =
        SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
    moment_fmt_ = SDL_GPU_TEXTUREFORMAT_R16G16_FLOAT;

    for (int k = 0; k < num_cascades_; ++k) {
        SDL_GPUTextureCreateInfo ti{};
        ti.type                 = SDL_GPU_TEXTURETYPE_2D;
        ti.format               = moment_fmt_;
        ti.width                = (uint32_t)map_size;
        ti.height               = (uint32_t)map_size;
        ti.layer_count_or_depth = 1;
        ti.num_levels           = 1;
        ti.usage                = ct_usage;
        moment_tex_[k] = GpuCreateTexture(dev, &ti);
        if (!moment_tex_[k]) {
            fprintf(stderr, "[EvsmShadow] moment texture[%d] creation failed: %s\n",
                    k, SDL_GetError());
            return false;
        }
    }

    // ── Bilinear sampler (linear filtering on RG32F is safe and improves softness) ─
    {
        SDL_GPUSamplerCreateInfo si{};
        si.min_filter     = SDL_GPU_FILTER_LINEAR;
        si.mag_filter     = SDL_GPU_FILTER_LINEAR;
        si.mipmap_mode    = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
        si.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
        si.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
        si.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
        sampler_ = GpuCreateSampler(dev, &si);
    }

    // ── Moment-write pipeline ─────────────────────────────────────────────────
    // Vert: shadow_csm.vert — same light-space transform as CSM depth pass.
    // Frag: evsm_moments.frag — writes (exp(c*z), exp(2c*z)) to RG color target.
    {
        SDL_GPUShader* vert = LoadSpv(dev, "shaders/spirv/shadow_csm.vert.spv",
                                       SDL_GPU_SHADERSTAGE_VERTEX, 1, 0);
        SDL_GPUShader* frag = LoadSpv(dev, "shaders/spirv/evsm_moments.frag.spv",
                                       SDL_GPU_SHADERSTAGE_FRAGMENT, 1, 0);
        if (!vert || !frag) {
            if (vert) GpuReleaseShader(dev, vert);
            if (frag) GpuReleaseShader(dev, frag);
            fprintf(stderr, "[EvsmShadow] shader load failed\n");
        } else {
            // Vertex layout: position only (float3, stride 24 = pos+norm same as CSM).
            SDL_GPUVertexAttribute va{};
            va.location    = 0;
            va.buffer_slot = 0;
            va.format      = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3;
            va.offset      = 0;

            SDL_GPUVertexBufferDescription vbd{};
            vbd.slot       = 0;
            vbd.pitch      = 24;
            vbd.input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX;

            SDL_GPUVertexInputState vis{};
            vis.vertex_buffer_descriptions = &vbd;
            vis.num_vertex_buffers         = 1;
            vis.vertex_attributes          = &va;
            vis.num_vertex_attributes      = 1;

            SDL_GPUColorTargetDescription ct{};
            ct.format = moment_fmt_; // probed above via SDL_GPUTextureSupportsFormat

            SDL_GPUGraphicsPipelineTargetInfo tgt{};
            tgt.color_target_descriptions = &ct;
            tgt.num_color_targets         = 1;
            tgt.has_depth_stencil_target  = false;

            SDL_GPURasterizerState rast{};
            rast.cull_mode  = SDL_GPU_CULLMODE_NONE;
            rast.fill_mode  = SDL_GPU_FILLMODE_FILL;
            rast.front_face = SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE;

            SDL_GPUDepthStencilState ds{};
            ds.enable_depth_test  = false;
            ds.enable_depth_write = false;

            SDL_GPUGraphicsPipelineCreateInfo ci{};
            ci.vertex_shader       = vert;
            ci.fragment_shader     = frag;
            ci.vertex_input_state  = vis;
            ci.rasterizer_state    = rast;
            ci.depth_stencil_state = ds;
            ci.target_info         = tgt;
            ci.primitive_type      = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;

            moment_pipeline_ = GpuCreateGraphicsPipeline(dev, &ci);
            GpuReleaseShader(dev, vert);
            GpuReleaseShader(dev, frag);

            if (!moment_pipeline_)
                fprintf(stderr, "[EvsmShadow] moment pipeline failed: %s\n",
                        SDL_GetError());
        }
    }

    ready_ = (moment_tex_[0] && sampler_);

    // ── VBfA R-3: blur temp + output textures + fragment pipeline ────────────
    // Fragment-shader fullscreen-triangle blur since 2026-08-09 (replaced the
    // shadow_blur_h/v.comp compute pair -- see ApplyBlur's own doc comment).
    // Design unchanged from the compute version: moment_tex_ stays
    // COLOR_TARGET|SAMPLER only, read-only here.
    //   H-pass: moment_tex_[k] (sampler) → blur_tmp_[k] (COLOR_TARGET)
    //   V-pass: blur_tmp_[k]  (sampler) → blur_out_[k] (COLOR_TARGET)
    //   BindArrayForSampling() binds blur_out_[k] when blur_ready_.
    // blur_tmp_/blur_out_ reuse moment_fmt_ directly -- already proven to
    // support COLOR_TARGET usage (moment_tex_ itself uses it), so the old
    // R32G32_FLOAT/R16G16_FLOAT COMPUTE_STORAGE_WRITE capability probe (and
    // the Intel ANV crash it was guarding against, specific to that usage
    // flag) no longer applies.
    {
        bool all_ok = true;
        for (int k = 0; k < num_cascades_; ++k) {
            SDL_GPUTextureCreateInfo ti{};
            ti.type                 = SDL_GPU_TEXTURETYPE_2D;
            ti.format               = moment_fmt_;
            ti.width                = (uint32_t)map_size;
            ti.height               = (uint32_t)map_size;
            ti.layer_count_or_depth = 1;
            ti.num_levels           = 1;
            ti.usage                = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET |
                                       SDL_GPU_TEXTUREUSAGE_SAMPLER;
            blur_tmp_[k] = GpuCreateTexture(dev, &ti);
            blur_out_[k] = GpuCreateTexture(dev, &ti);
            if (!blur_tmp_[k] || !blur_out_[k]) {
                fprintf(stderr, "[EvsmShadow] blur texture[%d] failed: %s\n",
                        k, SDL_GetError());
                all_ok = false;
            }
        }
        if (all_ok) {
            GpuPipeline::Desc bd;
            bd.vert_path        = "shaders/deferred_lighting.vert"; // shared fullscreen tri
            bd.frag_path        = "shaders/shadow_blur.frag";
            bd.layout.count     = 0;   // gl_VertexIndex only, no vertex buffer
            bd.layout.stride    = 0;
            bd.raster.depth_test  = false;
            bd.raster.depth_write = false;
            bd.raster.cull_back   = false;
            bd.has_depth_target   = false;
            bd.frag_uniform_bufs  = 1;   // BlurUBO
            bd.frag_samplers      = 1;   // src_moments
            bd.color_format       = moment_fmt_;
            blur_pipeline_.Create(bd);

            blur_ready_ = blur_pipeline_.SDLPipeline() != nullptr;
        }
    }

    fprintf(stdout, "[EvsmShadow] %d cascades, %dx%d, c=%.1f, pipeline=%s, blur=%s\n",
            num_cascades_, map_size, map_size, (double)warp_c,
            moment_pipeline_ ? "ok" : "null",
            blur_ready_       ? "ok" : "null");
    return ready_;
}

void EvsmShadow::Shutdown() {
    md::GpuDeviceHandle dev = GpuDevice::Get().SDLDevice();
    if (!dev) return;
    if (moment_pipeline_) {
        GpuReleaseGraphicsPipeline(dev, moment_pipeline_);
        moment_pipeline_ = nullptr;
    }
    for (int k = 0; k < NUM_CASCADES; ++k) {
        if (moment_tex_[k]) {
            GpuReleaseTexture(dev, moment_tex_[k]);
            moment_tex_[k] = nullptr;
        }
        if (blur_tmp_[k]) {
            GpuReleaseTexture(dev, blur_tmp_[k]);
            blur_tmp_[k] = nullptr;
        }
        if (blur_out_[k]) {
            GpuReleaseTexture(dev, blur_out_[k]);
            blur_out_[k] = nullptr;
        }
    }
    blur_pipeline_.Destroy();
    if (sampler_) { GpuReleaseSampler(dev, sampler_); sampler_ = nullptr; }
    ready_      = false;
    blur_ready_ = false;
}

// ── VBfA R-3: Gaussian blur ────────────────────────────────────────────────────
// Fragment-shader fullscreen-triangle pass since 2026-08-09 (was 2 compute
// dispatches, shadow_blur_h/v.comp -- removed as part of shrinking the
// Filament-migration-blocker-1 (compute) surface; see
// docs/analysis/FILAMENT_MIGRATION_ANALYSIS.md). Same math, same ping-pong
// texture flow, only the dispatch mechanism changed: a render pass writing
// a COLOR_TARGET instead of a compute pass writing a storage image.
// H: moment_tex_[k] (sampler) → blur_tmp_[k] (COLOR_TARGET)
// V: blur_tmp_[k]  (sampler) → blur_out_[k] (COLOR_TARGET)
// moment_tex_ is read-only here. BindArrayForSampling() serves blur_out_[k]
// instead of moment_tex_[k] once blur_ready_.
void EvsmShadow::ApplyBlur(md::GpuCommandBufferHandle cmd) {
    if (!cmd || !blur_ready_) return;

    struct alignas(16) BlurUBO { float texel[2]; float horizontal; float _pad; };
    const float texel = 1.f / (float)map_size_;

    for (int k = 0; k < num_cascades_; ++k) {
        if (!moment_tex_[k] || !blur_tmp_[k] || !blur_out_[k]) continue;

        // ── H pass: moment_tex_[k] → blur_tmp_[k] ────────────────────────────
        {
            GpuCommandBuffer cb;
            GpuCommandBuffer::ColorPassDesc cpd;
            cpd.cmd       = cmd;
            cpd.color_tex[0] = blur_tmp_[k];
            cb.BeginColorPass(cpd);
            cb.BindPipeline(&blur_pipeline_);
            SDL_GPUTextureSamplerBinding tsb{ moment_tex_[k], sampler_ };
            cb.BindFragmentSamplers(0, &tsb, 1);
            BlurUBO ubo{ {texel, texel}, 1.f, 0.f };
            cb.PushFragmentUniforms(0, &ubo, sizeof(ubo));
            cb.Draw(3, 0);
            cb.EndPass();
        }
        // ── V pass: blur_tmp_[k] → blur_out_[k] ──────────────────────────────
        {
            GpuCommandBuffer cb;
            GpuCommandBuffer::ColorPassDesc cpd;
            cpd.cmd       = cmd;
            cpd.color_tex[0] = blur_out_[k];
            cb.BeginColorPass(cpd);
            cb.BindPipeline(&blur_pipeline_);
            SDL_GPUTextureSamplerBinding tsb{ blur_tmp_[k], sampler_ };
            cb.BindFragmentSamplers(0, &tsb, 1);
            BlurUBO ubo{ {texel, texel}, 0.f, 0.f };
            cb.PushFragmentUniforms(0, &ubo, sizeof(ubo));
            cb.Draw(3, 0);
            cb.EndPass();
        }
    }
}

// ── Shadow pass ───────────────────────────────────────────────────────────────

SDL_GPURenderPass* EvsmShadow::BeginMomentPass(md::GpuCommandBufferHandle cmd, int cascade) {
    if (!ready_ || cascade < 0 || cascade >= num_cascades_) return nullptr;

    SDL_GPUColorTargetInfo ct{};
    ct.texture     = moment_tex_[cascade];
    ct.load_op     = SDL_GPU_LOADOP_CLEAR;
    ct.store_op    = SDL_GPU_STOREOP_STORE;
    ct.clear_color = { 0.f, 0.f, 0.f, 1.f };

    moment_pass_ = SDL_BeginGPURenderPass(cmd, &ct, 1, nullptr);
    return moment_pass_;
}

void EvsmShadow::EndMomentPass() {
    if (moment_pass_) {
        SDL_EndGPURenderPass(moment_pass_);
        moment_pass_ = nullptr;
    }
}

// ── Sampling ──────────────────────────────────────────────────────────────────

void EvsmShadow::BindArrayForSampling(SDL_GPURenderPass* pass, uint32_t slot) {
    if (!ready_ || !pass) return;
    // When blur is ready, serve blur_out_ (Gaussian-blurred moments).
    // Otherwise, fall back to unblurred moment_tex_ (blur disabled or not supported).
    SDL_GPUTextureSamplerBinding tsb[NUM_CASCADES];
    for (int k = 0; k < num_cascades_; ++k) {
        md::GpuTextureHandle tex = (blur_ready_ && blur_out_[k]) ? blur_out_[k] : moment_tex_[k];
        tsb[k] = { tex, sampler_ };
    }
    SDL_BindGPUFragmentSamplers(pass, slot, tsb, (uint32_t)num_cascades_);
}

} // namespace md
#endif // MD_SDL_GPU

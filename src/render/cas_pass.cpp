#include <monkey_dust/render/cas_pass.h>
#ifdef MD_SDL_GPU
#include <monkey_dust/render/gpu_device.h>
#include <monkey_dust/render/gpu_pipeline.h>
#include <monkey_dust/render/gpu_hal_free_functions.h>
#include <cstdio>

namespace md {

// ── Texture management ────────────────────────────────────────────────────────

void CasPass::CreateTextures(int w, int h) {
    md::GpuDeviceHandle dev = GpuDevice::Get().SDLDevice();
    if (!dev) return;

    // Scene color texture: R8G8B8A8_UNORM, sampleable + color target.
    SDL_GPUTextureCreateInfo ti{};
    ti.type                 = SDL_GPU_TEXTURETYPE_2D;
    ti.format               = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    ti.width                = (uint32_t)w;
    ti.height               = (uint32_t)h;
    ti.layer_count_or_depth = 1;
    ti.num_levels           = 1;
    ti.usage                = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET |
                              SDL_GPU_TEXTUREUSAGE_SAMPLER;
    scene_tex_ = GpuCreateTexture(dev, &ti);

    // Depth texture for scene render. D32_FLOAT: Intel Gen9 cannot sample D24_UNORM.
    ti.format = SDL_GPU_TEXTUREFORMAT_D32_FLOAT;
    ti.usage   = SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET;
    depth_tex_ = GpuCreateTexture(dev, &ti);

    // Nearest sampler for CAS input (no interpolation — read exact pixels).
    SDL_GPUSamplerCreateInfo si{};
    si.min_filter = SDL_GPU_FILTER_NEAREST;
    si.mag_filter = SDL_GPU_FILTER_NEAREST;
    si.mipmap_mode  = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
    si.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    si.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    si.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    sampler_ = GpuCreateSampler(dev, &si);

    tex_w_ = w;
    tex_h_ = h;
}

void CasPass::DestroyTextures() {
    md::GpuDeviceHandle dev = GpuDevice::Get().SDLDevice();
    if (!dev) return;
    if (scene_tex_) { GpuReleaseTexture(dev, scene_tex_); scene_tex_ = nullptr; }
    if (depth_tex_) { GpuReleaseTexture(dev, depth_tex_); depth_tex_ = nullptr; }
    if (sampler_)   { GpuReleaseSampler(dev, sampler_);   sampler_   = nullptr; }
}

// ── Init / Shutdown / Resize ──────────────────────────────────────────────────

bool CasPass::Init(int vp_w, int vp_h, float sharpness) {
    sharpness_ = sharpness;
    CreateTextures(vp_w, vp_h);

    // CAS pipeline — full-screen triangle, no vertex input.
    GpuPipeline::Desc pd;
    pd.vert_path = "shaders/cas.vert";
    pd.frag_path = "shaders/cas.frag";
    // No vertex attributes (gl_VertexIndex only).
    pd.layout.count  = 0;
    pd.layout.stride = 0;
    pd.raster.depth_test  = false;
    pd.raster.depth_write = false;
    pd.raster.cull_back   = false;
    pd.has_depth_target   = false;
    pd.frag_uniform_bufs  = 1;   // CasUBO
    pd.frag_samplers      = 1;   // u_scene
    // CAS output format matches swapchain (use B8G8R8A8_UNORM — most common).
    // SDL3 on Linux/Vulkan typically uses B8G8R8A8_UNORM for swapchain.
    pd.color_format = SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM;

    if (!pipeline_.Create(pd)) {
        fprintf(stderr, "[CasPass] pipeline create failed\n");
        return false;
    }

    ready_ = true;
    fprintf(stdout, "[CasPass] init %dx%d sharpness=%.2f\n", vp_w, vp_h, sharpness_);
    return true;
}

void CasPass::Shutdown() {
    DestroyTextures();
    pipeline_.Destroy();
    ready_ = false;
}

void CasPass::Resize(int vp_w, int vp_h) {
    if (vp_w == tex_w_ && vp_h == tex_h_) return;
    DestroyTextures();
    CreateTextures(vp_w, vp_h);
}

// ── Apply ─────────────────────────────────────────────────────────────────────

void CasPass::Apply(md::GpuCommandBufferHandle cmd, md::GpuTextureHandle output_tex,
                     int vp_w, int vp_h) {
    if (!ready_ || !scene_tex_ || !output_tex) return;

    Resize(vp_w, vp_h);

    // Open render pass on swapchain (output_tex), no depth.
    GpuCommandBuffer cb;
    GpuCommandBuffer::ColorPassDesc cpd;
    cpd.cmd        = cmd;
    cpd.color_tex[0]  = output_tex;
    cpd.depth_tex  = nullptr;
    cpd.load_color = false;   // CAS replaces the output — clear is fine
    cb.BeginColorPass(cpd);

    cb.BindPipeline(&pipeline_);

    // Bind scene texture + sampler.
    SDL_GPUTextureSamplerBinding tsb{ scene_tex_, sampler_ };
    cb.BindFragmentSamplers(0, &tsb, 1);

    // Push CasUBO: texel size + sharpness.
    struct CasUBO {
        float texel_x, texel_y;
        float sharpness;
        float _pad;
    } ubo;
    ubo.texel_x   = 1.f / (float)vp_w;
    ubo.texel_y   = 1.f / (float)vp_h;
    ubo.sharpness = sharpness_;
    ubo._pad      = 0.f;
    cb.PushFragmentUniforms(0, &ubo, sizeof(ubo));

    // Full-screen triangle: 3 vertices, no vertex buffer.
    cb.Draw(3, 0);
    cb.EndPass();
}

} // namespace md
#endif // MD_SDL_GPU

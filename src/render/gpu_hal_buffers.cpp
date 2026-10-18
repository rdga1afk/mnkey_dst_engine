#include "gpu_hal_buffers_internal.h"

// ── GpuVertexBuffer ───────────────────────────────────────────────────────────

void GpuVertexBuffer::Init(uint32_t max_vertices, uint32_t vertex_stride) {
    stride_ = vertex_stride;
    sdl_size_ = max_vertices * vertex_stride;
    SDL_GPUDevice* dev = md::GpuDevice::Get().SDLDevice();

    SDL_GPUBufferCreateInfo buf_info = {};
    buf_info.usage = SDL_GPU_BUFFERUSAGE_VERTEX;
    buf_info.size  = sdl_size_;
    sdl_buf_ = SDL_CreateGPUBuffer(dev, &buf_info);
    if (!sdl_buf_)
        MD_LOG(MD_LOG_WARNING, "[GpuVertexBuffer] SDL_CreateGPUBuffer failed: %s", SDL_GetError());

    SDL_GPUTransferBufferCreateInfo tbuf_info = {};
    tbuf_info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    tbuf_info.size  = sdl_size_;
    sdl_transfer_ = SDL_CreateGPUTransferBuffer(dev, &tbuf_info);
    if (!sdl_transfer_)
        MD_LOG(MD_LOG_WARNING, "[GpuVertexBuffer] SDL_CreateGPUTransferBuffer failed: %s", SDL_GetError());
}

void GpuVertexBuffer::Shutdown() {
    SDL_GPUDevice* dev = md::GpuDevice::Get().SDLDevice();
    if (sdl_buf_)      { SDL_ReleaseGPUBuffer(dev, sdl_buf_);                sdl_buf_      = nullptr; }
    if (sdl_transfer_) { SDL_ReleaseGPUTransferBuffer(dev, sdl_transfer_);   sdl_transfer_ = nullptr; }
    sdl_size_ = 0;
    stride_ = 0;
}

void* GpuVertexBuffer::MapWrite() {
    if (!sdl_transfer_) return nullptr;
    return SDL_MapGPUTransferBuffer(md::GpuDevice::Get().SDLDevice(),
                                    sdl_transfer_, true /*cycle*/);
}

void GpuVertexBuffer::Unmap() {
    if (sdl_transfer_)
        SDL_UnmapGPUTransferBuffer(md::GpuDevice::Get().SDLDevice(), sdl_transfer_);
}

void GpuVertexBuffer::Upload(SDL_GPUCommandBuffer* cmd) {
    if (!cmd || !sdl_buf_ || !sdl_transfer_) return;
    SDL_GPUCopyPass* pass = SDL_BeginGPUCopyPass(cmd);
    SDL_GPUTransferBufferLocation src = {};
    src.transfer_buffer = sdl_transfer_;
    src.offset          = 0;
    SDL_GPUBufferRegion dst = {};
    dst.buffer = sdl_buf_;
    dst.offset = 0;
    dst.size   = sdl_size_;
    SDL_UploadToGPUBuffer(pass, &src, &dst, true /*cycle*/);
    SDL_EndGPUCopyPass(pass);
}

void GpuVertexBuffer::Advance() {
    // SDL_GPU: cycle=true in MapWrite/Upload handles versioning — no explicit advance needed.
}

// ── GpuDepthTexture ───────────────────────────────────────────────────────────

void GpuDepthTexture::Init(int w, int h, bool shadow_border) {
    w_ = w; h_ = h;
    SDL_GPUDevice* dev = md::GpuDevice::Get().SDLDevice();

    SDL_GPUTextureCreateInfo ti = {};
    ti.type                  = SDL_GPU_TEXTURETYPE_2D;
    // D32_SFLOAT is universally supported for SAMPLER usage in Vulkan (D24_UNORM is not on Intel Gen9).
    ti.format                = SDL_GPU_TEXTUREFORMAT_D32_FLOAT;
    ti.usage                 = SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET |
                               SDL_GPU_TEXTUREUSAGE_SAMPLER;
    ti.width                 = (Uint32)w;
    ti.height                = (Uint32)h;
    ti.layer_count_or_depth  = 1;
    ti.num_levels            = 1;
    sdl_tex_ = SDL_CreateGPUTexture(dev, &ti);
    if (!sdl_tex_) {
        MD_LOG(MD_LOG_WARNING, "[GpuDepthTexture] SDL_CreateGPUTexture failed: %s", SDL_GetError());
        return;
    }
    // SDL3 has no CLAMP_TO_BORDER — use CLAMP_TO_EDGE (minor shadow-edge artefact).
    SDL_GPUSamplerCreateInfo si = {};
    si.min_filter     = SDL_GPU_FILTER_LINEAR;
    si.mag_filter     = SDL_GPU_FILTER_LINEAR;
    si.mipmap_mode    = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
    si.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    si.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    si.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    si.min_lod        = 0.0f;
    si.max_lod        = 0.0f;
    sdl_sampler_ = SDL_CreateGPUSampler(dev, &si);
    md::GpuResourceTracker::Get().OnDepthTextureCreate();
    (void)shadow_border;
}

void GpuDepthTexture::Shutdown() {
    SDL_GPUDevice* dev = md::GpuDevice::Get().SDLDevice();
    if (sdl_sampler_) { SDL_ReleaseGPUSampler(dev, sdl_sampler_); sdl_sampler_ = nullptr; }
    if (sdl_tex_)     {
        SDL_ReleaseGPUTexture(dev, sdl_tex_);
        sdl_tex_ = nullptr;
        md::GpuResourceTracker::Get().OnDepthTextureDestroy();
    }
    w_ = h_ = 0;
}

void GpuDepthTexture::Bind(uint32_t unit) const {
    (void)unit; // SDL_GPU: binding via SDL_BindGPUFragmentSamplers in render pass (Step 6)
}

// ── GpuSampler ────────────────────────────────────────────────────────────────

bool GpuSampler::Init(const GpuSamplerDesc& desc) {
    SDL_GPUDevice* dev = md::GpuDevice::Get().SDLDevice();
    if (!dev) return false;
    sdl_sampler_ = CreateSDLSampler(dev, desc);
    if (!sdl_sampler_) {
        MD_LOG(MD_LOG_WARNING, "[GpuSampler] SDL_CreateGPUSampler failed: %s", SDL_GetError());
        return false;
    }
    return true;
}

void GpuSampler::Shutdown() {
    if (sdl_sampler_) {
        SDL_ReleaseGPUSampler(md::GpuDevice::Get().SDLDevice(), sdl_sampler_);
        sdl_sampler_ = nullptr;
    }
}

// ── GpuColorTexture ───────────────────────────────────────────────────────────

bool GpuColorTexture::Init(int w, int h, SDL_GPUTextureFormat format,
                            SDL_GPUTextureUsageFlags usage, uint32_t num_levels) {
    w_ = w; h_ = h;
    SDL_GPUDevice* dev = md::GpuDevice::Get().SDLDevice();
    if (!dev) return false;

    SDL_GPUTextureCreateInfo ti = {};
    ti.type                 = SDL_GPU_TEXTURETYPE_2D;
    ti.format               = format;
    ti.usage                = usage;
    ti.width                = (Uint32)w;
    ti.height               = (Uint32)h;
    ti.layer_count_or_depth = 1;
    ti.num_levels           = num_levels;
    sdl_tex_ = SDL_CreateGPUTexture(dev, &ti);
    if (!sdl_tex_) {
        MD_LOG(MD_LOG_WARNING, "[GpuColorTexture] SDL_CreateGPUTexture failed: %s", SDL_GetError());
        return false;
    }
    md::GpuResourceTracker::Get().OnTextureCreate();
    return true;
}

void GpuColorTexture::Shutdown() {
    if (sdl_tex_) {
        SDL_ReleaseGPUTexture(md::GpuDevice::Get().SDLDevice(), sdl_tex_);
        sdl_tex_ = nullptr;
        md::GpuResourceTracker::Get().OnTextureDestroy();
    }
    w_ = h_ = 0;
}

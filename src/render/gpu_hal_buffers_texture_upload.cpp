#include "gpu_hal_buffers_internal.h"
#include <monkey_dust/render/gpu_hal_free_functions.h>

#ifdef MD_SDL_GPU
bool GpuTexture::InitRenderTarget(int w, int h, const GpuSamplerDesc& s, SDL_GPUTextureFormat format) {
#else
bool GpuTexture::InitRenderTarget(int w, int h, const GpuSamplerDesc& s) {
#endif
    w_ = w; h_ = h;
#ifdef MD_SDL_GPU
    SDL_GPUDevice* dev = md::GpuDevice::Get().SDLDevice();
    if (!dev) return false;
    uint32_t num_levels = s.gen_mipmap ? MipLevels(w, h) : 1u;

    SDL_GPUTextureCreateInfo ti = {};
    ti.type                 = SDL_GPU_TEXTURETYPE_2D;
    ti.format               = format == SDL_GPU_TEXTUREFORMAT_INVALID
                             ? SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM : format;
    ti.usage                = SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COLOR_TARGET;
    ti.width                = (Uint32)w;
    ti.height                = (Uint32)h;
    ti.layer_count_or_depth = 1;
    ti.num_levels           = num_levels;
    sdl_tex_ = SDL_CreateGPUTexture(dev, &ti);
    if (!sdl_tex_) {
        MD_LOG(MD_LOG_WARNING, "[GpuTexture] InitRenderTarget SDL_CreateGPUTexture failed: %s", SDL_GetError());
        return false;
    }
    md::GpuResourceTracker::Get().OnTextureCreate();
    sdl_sampler_ = CreateSDLSampler(dev, s);
    return true;
#else
    return false;
#endif
}

bool GpuTexture::InitCompute(int w, int h, SDL_GPUTextureFormat format,
                              SDL_GPUTextureUsageFlags usage, uint32_t num_levels,
                              const GpuSamplerDesc& s) {
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
        MD_LOG(MD_LOG_WARNING, "[GpuTexture] InitCompute SDL_CreateGPUTexture failed: %s", SDL_GetError());
        return false;
    }
    md::GpuResourceTracker::Get().OnTextureCreate();
    sdl_sampler_ = CreateSDLSampler(dev, s);
    return true;
}

bool GpuTexture::InitFromMemory(const uint8_t* rgba8, int w, int h, const GpuSamplerDesc& s) {
    w_ = w; h_ = h;
#ifdef MD_SDL_GPU
    SDL_GPUDevice* dev = md::GpuDevice::Get().SDLDevice();
    if (dev) {
        uint32_t num_levels = s.gen_mipmap ? MipLevels(w, h) : 1u;
        SDL_GPUTextureUsageFlags usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
        if (s.gen_mipmap) usage |= SDL_GPU_TEXTUREUSAGE_COLOR_TARGET;

        SDL_GPUTextureCreateInfo ti = {};
        ti.type                 = SDL_GPU_TEXTURETYPE_2D;
        ti.format               = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
        ti.usage                = usage;
        ti.width                = (Uint32)w;
        ti.height                = (Uint32)h;
        ti.layer_count_or_depth = 1;
        ti.num_levels           = num_levels;
        sdl_tex_ = SDL_CreateGPUTexture(dev, &ti);
        if (!sdl_tex_) {
            MD_LOG(MD_LOG_WARNING, "[GpuTexture] SDL_CreateGPUTexture failed: %s", SDL_GetError());
            return false;
        }
        md::GpuResourceTracker::Get().OnTextureCreate();

        uint32_t upload_size = (uint32_t)(w * h * 4);
        SDL_GPUTransferBufferCreateInfo tbuf = {};
        tbuf.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
        tbuf.size  = upload_size;
        SDL_GPUTransferBuffer* transfer = SDL_CreateGPUTransferBuffer(dev, &tbuf);
        void* map = GpuMapTransfer(transfer, false);
        if (map) { memcpy(map, rgba8, upload_size); GpuUnmapTransfer(transfer); }

        SDL_GPUCommandBuffer* cmd  = SDL_AcquireGPUCommandBuffer(dev);
        if (!cmd) { MD_LOG(MD_LOG_WARNING, "[GpuTexture] AcquireCmd failed: %s", SDL_GetError()); SDL_ReleaseGPUTransferBuffer(dev, transfer); return false; }
        SDL_GPUCopyPass*      pass = SDL_BeginGPUCopyPass(cmd);
        SDL_GPUTextureTransferInfo src_info = {};
        src_info.transfer_buffer = transfer;
        src_info.pixels_per_row  = (Uint32)w;
        src_info.rows_per_layer  = (Uint32)h;
        SDL_GPUTextureRegion dst_region = {};
        dst_region.texture = sdl_tex_;
        dst_region.w       = (Uint32)w;
        dst_region.h       = (Uint32)h;
        dst_region.d       = 1;
        SDL_UploadToGPUTexture(pass, &src_info, &dst_region, false);
        SDL_EndGPUCopyPass(pass);
        if (s.gen_mipmap) SDL_GenerateMipmapsForGPUTexture(cmd, sdl_tex_);
        if (!SDL_SubmitGPUCommandBuffer(cmd))
            MD_LOG(MD_LOG_WARNING, "[GpuTexture] submit failed: %s", SDL_GetError());
        SDL_ReleaseGPUTransferBuffer(dev, transfer);

        sdl_sampler_ = CreateSDLSampler(dev, s);
        return true;
    }
    // dev == null: SDL_GPU not initialised — fall through to OpenGL path below.
#endif
    glGenTextures(1, &id_);
    glBindTexture(GL_TEXTURE_2D, id_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba8);
    ApplySampler(s);
    glBindTexture(GL_TEXTURE_2D, 0);
    return true;
}

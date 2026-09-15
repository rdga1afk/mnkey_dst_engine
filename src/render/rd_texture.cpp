#include <monkey_dust/render/rd_texture.h>
#ifdef MD_SDL_GPU
#include <monkey_dust/render/rd_device.h>
#include <monkey_dust/render/gpu_device.h>
#include <monkey_dust/render/gpu_copy_pass.h>
#include <monkey_dust/render/gpu_hal_free_functions.h>
#include <cstdio>
#include <cstring>
#include "stb_image.h"

namespace md {

static uint32_t MipCount(uint32_t w, uint32_t h) {
    uint32_t n = 1;
    while (w > 1 || h > 1) {
        if (w > 1) w >>= 1;
        if (h > 1) h >>= 1;
        ++n;
    }
    return n;
}

bool RdTexture::Upload(const uint8_t* rgba8, const Desc& d) {
    md::GpuDeviceHandle dev = md::GpuDevice::Get().SDLDevice();
    if (!dev || !rgba8 || !d.width || !d.height) return false;

    mip_levels_ = d.gen_mipmaps ? MipCount(d.width, d.height) : 1u;

    SDL_GPUTextureUsageFlags usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
    if (d.gen_mipmaps) usage |= SDL_GPU_TEXTUREUSAGE_COLOR_TARGET;

    SDL_GPUTextureCreateInfo ti{};
    ti.type                 = SDL_GPU_TEXTURETYPE_2D;
    ti.format               = d.format;
    ti.usage                = usage;
    ti.width                = d.width;
    ti.height               = d.height;
    ti.layer_count_or_depth = 1;
    ti.num_levels           = mip_levels_;
    tex_ = SDL_CreateGPUTexture(dev, &ti);
    if (!tex_) {
        fprintf(stderr, "[RdTexture] SDL_CreateGPUTexture failed: %s\n", SDL_GetError());
        mip_levels_ = 1;
        return false;
    }

    uint32_t upload_size = d.width * d.height * 4;
    SDL_GPUTransferBufferCreateInfo tbi{};
    tbi.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    tbi.size  = upload_size;
    SDL_GPUTransferBuffer* tb = SDL_CreateGPUTransferBuffer(dev, &tbi);
    void* map = GpuMapTransfer(tb, false);
    if (map) memcpy(map, rgba8, upload_size);
    GpuUnmapTransfer(tb);

    md::GpuCommandBufferHandle cmd = md::GpuDevice::Get().AcquireCommandBuffer();
    GpuCopyPass cp;
    cp.Begin(cmd);
    SDL_GPUTextureTransferInfo src{};
    src.transfer_buffer = tb;
    src.pixels_per_row  = d.width;
    src.rows_per_layer  = d.height;
    SDL_GPUTextureRegion dst{};
    dst.texture = tex_;
    dst.w       = d.width;
    dst.h       = d.height;
    dst.d       = 1;
    cp.UploadTexture(src, dst, false);
    cp.End();
    if (d.gen_mipmaps && mip_levels_ > 1)
        GpuGenerateMipmaps(cmd, tex_);
    md::GpuDevice::Get().Submit(cmd);
    SDL_ReleaseGPUTransferBuffer(dev, tb);

    // Sampler: LINEAR_MIPMAP when mips>1, NEAREST when single-mip (Intel ANV workaround).
    SDL_GPUSamplerCreateInfo si{};
    si.min_filter     = SDL_GPU_FILTER_LINEAR;
    si.mag_filter     = SDL_GPU_FILTER_LINEAR;
    si.mipmap_mode    = (mip_levels_ > 1)
                        ? SDL_GPU_SAMPLERMIPMAPMODE_LINEAR
                        : SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
    si.address_mode_u = si.address_mode_v = si.address_mode_w =
        SDL_GPU_SAMPLERADDRESSMODE_REPEAT;
    si.max_lod        = (float)(mip_levels_ - 1);
    samp_ = SDL_CreateGPUSampler(dev, &si);
    if (!samp_) {
        fprintf(stderr, "[RdTexture] SDL_CreateGPUSampler failed: %s\n", SDL_GetError());
        SDL_ReleaseGPUTexture(dev, tex_); tex_ = nullptr;
        mip_levels_ = 1;
        return false;
    }

    fprintf(stdout, "[RdTexture] %ux%u mips=%u ok\n", d.width, d.height, mip_levels_);
    return true;
}

bool RdTexture::LoadFile(const char* path, bool gen_mipmaps, bool flip_v) {
    stbi_set_flip_vertically_on_load(flip_v ? 1 : 0);
    int w = 0, h = 0, ch = 0;
    uint8_t* data = stbi_load(path, &w, &h, &ch, 4);
    stbi_set_flip_vertically_on_load(0);
    if (!data) {
        fprintf(stderr, "[RdTexture] stbi_load failed '%s': %s\n",
                path, stbi_failure_reason());
        return false;
    }
    Desc d;
    d.width       = (uint32_t)w;
    d.height      = (uint32_t)h;
    d.gen_mipmaps = gen_mipmaps;
    bool ok = Upload(data, d);
    stbi_image_free(data);
    return ok;
}

void RdTexture::Release() {
    md::GpuDeviceHandle dev = md::GpuDevice::Get().SDLDevice();
    if (dev) {
        if (samp_) { SDL_ReleaseGPUSampler(dev, samp_); samp_ = nullptr; }
        if (tex_)  { SDL_ReleaseGPUTexture(dev, tex_);  tex_  = nullptr; }
    }
    mip_levels_ = 1;
}

} // namespace md
#endif // MD_SDL_GPU

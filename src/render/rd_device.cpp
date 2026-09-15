#include <monkey_dust/render/rd_device.h>
#ifdef MD_SDL_GPU
#include <monkey_dust/render/gpu_copy_pass.h>
#include <monkey_dust/render/gpu_hal_free_functions.h>
#include <monkey_dust/render/gpu_device.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace md {

static bool DetectIntelANV(md::GpuDeviceHandle dev) {
    const char* drv = SDL_GetGPUDeviceDriver(dev);
#ifdef __linux__
    // On Linux, Vulkan is used by Intel ANV, AMD RADV, and NVidia NVK/proprietary.
    // Without VkPhysicalDeviceProperties via SDL3, default to conservative Intel limits
    // for all Vulkan-on-Linux targets. Override via env: MD_GPU_LIBERAL=1.
    if (drv && strcmp(drv, "vulkan") == 0) {
        const char* liberal = getenv("MD_GPU_LIBERAL");
        if (liberal && liberal[0] == '1') return false;
        return true;
    }
#endif
    (void)drv;
    return false;
}

void RdDevice::Init(md::GpuDeviceHandle dev) {
    if (!dev) return;
    dev_ = dev;

    caps_.is_intel_anv = DetectIntelANV(dev);
    if (caps_.is_intel_anv) {
        caps_.max_frag_samplers     = 3;
        caps_.max_vert_storage_bufs = 4;
        fprintf(stdout, "[RdDevice] Intel ANV limits: frag_samplers=%d vert_storage=%d\n",
                caps_.max_frag_samplers, caps_.max_vert_storage_bufs);
    } else {
        caps_.max_frag_samplers     = 8;
        caps_.max_vert_storage_bufs = 8;
        fprintf(stdout, "[RdDevice] driver=%s liberal limits: frag_samplers=%d vert_storage=%d\n",
                SDL_GetGPUDeviceDriver(dev),
                caps_.max_frag_samplers, caps_.max_vert_storage_bufs);
    }

    BuildDummyResources(dev);
}

void RdDevice::BuildDummyResources(md::GpuDeviceHandle dev) {
    SDL_GPUTextureCreateInfo ti{};
    ti.type                 = SDL_GPU_TEXTURETYPE_2D;
    ti.format               = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    ti.usage                = SDL_GPU_TEXTUREUSAGE_SAMPLER;
    ti.width = ti.height    = 1;
    ti.layer_count_or_depth = 1;
    ti.num_levels           = 1;
    dummy_tex_ = SDL_CreateGPUTexture(dev, &ti);
    if (dummy_tex_) {
        static const uint8_t kWhite[4] = {255, 255, 255, 255};
        SDL_GPUTransferBufferCreateInfo tbi{};
        tbi.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
        tbi.size  = 4;
        SDL_GPUTransferBuffer* tb = SDL_CreateGPUTransferBuffer(dev, &tbi);
        void* p = GpuMapTransfer(tb, false);
        if (p) memcpy(p, kWhite, 4);
        GpuUnmapTransfer(tb);
        md::GpuCommandBufferHandle cmd = GpuDevice::Get().AcquireCommandBuffer();
        GpuCopyPass cp;
        cp.Begin(cmd);
        SDL_GPUTextureTransferInfo src{};
        src.transfer_buffer = tb; src.pixels_per_row = 1; src.rows_per_layer = 1;
        SDL_GPUTextureRegion dst{};
        dst.texture = dummy_tex_; dst.w = dst.h = dst.d = 1;
        cp.UploadTexture(src, dst, false);
        cp.End();
        GpuDevice::Get().Submit(cmd);
        SDL_ReleaseGPUTransferBuffer(dev, tb);
    }

    SDL_GPUSamplerCreateInfo si{};
    si.min_filter     = SDL_GPU_FILTER_NEAREST;
    si.mag_filter     = SDL_GPU_FILTER_NEAREST;
    si.mipmap_mode    = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
    si.address_mode_u = si.address_mode_v = si.address_mode_w =
        SDL_GPU_SAMPLERADDRESSMODE_REPEAT;
    dummy_samp_ = SDL_CreateGPUSampler(dev, &si);
}

void RdDevice::Shutdown() {
    if (!dev_) return;
    if (dummy_tex_)  { SDL_ReleaseGPUTexture(dev_, dummy_tex_);  dummy_tex_  = nullptr; }
    if (dummy_samp_) { SDL_ReleaseGPUSampler(dev_, dummy_samp_); dummy_samp_ = nullptr; }
    dev_ = nullptr;
}

} // namespace md
#endif // MD_SDL_GPU

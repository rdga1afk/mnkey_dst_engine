#include <monkey_dust/render/ssbo.h>
#include <cstdio>
#include <cstring>

#ifdef MD_SDL_GPU
#include <monkey_dust/render/gpu_device.h>
#include <monkey_dust/render/gpu_copy_pass.h>
#include <monkey_dust/render/gpu_hal_free_functions.h>
#include <monkey_dust/platform/md_log.h>
#endif


void SSBO::Init(int capacity_bytes, uint32_t extra_sdl_usage) {
#ifdef MD_SDL_GPU
    sdl_cap_ = (uint32_t)capacity_bytes;
    md::GpuDeviceHandle dev = md::GpuDevice::Get().SDLDevice();
    if (dev && capacity_bytes > 0) {
        SDL_GPUBufferCreateInfo bi = {};
        bi.usage = SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ |
                   SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_WRITE |
                   extra_sdl_usage;
        bi.size  = (Uint32)capacity_bytes;

        SDL_GPUTransferBufferCreateInfo ti = {};
        ti.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
        ti.size  = (Uint32)capacity_bytes;

        for (int s = 0; s < 3; ++s) {
            sdl_buf_[s] = SDL_CreateGPUBuffer(dev, &bi);
            if (!sdl_buf_[s])
                MD_LOG(MD_LOG_WARNING, "[SSBO] SDL_CreateGPUBuffer[%d] failed: %s", s, SDL_GetError());

            sdl_transfer_[s] = GpuCreateTransferBuffer(dev, &ti);
            if (!sdl_transfer_[s])
                MD_LOG(MD_LOG_WARNING, "[SSBO] SDL_CreateGPUTransferBuffer[%d] failed: %s", s, SDL_GetError());
        }
    }
#endif
}

// One-shot upload for static SSBOs: fill all 3 slots with the same data.
void SSBO::Upload(const void* data, int size_bytes, int offset) {
#ifdef MD_SDL_GPU
    md::GpuDeviceHandle dev = md::GpuDevice::Get().SDLDevice();
    if (!dev || !data || size_bytes <= 0) return;
    for (int s = 0; s < 3; ++s) {
        if (!sdl_buf_[s] || !sdl_transfer_[s]) continue;
        void* map = GpuMapTransfer(sdl_transfer_[s], false);
        if (map) {
            memcpy((uint8_t*)map + offset, data, (size_t)size_bytes);
            GpuUnmapTransfer(sdl_transfer_[s]);
        }
        md::GpuCommandBufferHandle cmd = md::GpuDevice::Get().AcquireCommandBuffer();
        GpuCopyPass pass;
        pass.Begin(cmd);
        SDL_GPUTransferBufferLocation src = {};
        src.transfer_buffer = sdl_transfer_[s];
        src.offset          = (Uint32)offset;
        SDL_GPUBufferRegion dst_r = {};
        dst_r.buffer = sdl_buf_[s];
        dst_r.offset = (Uint32)offset;
        dst_r.size   = (Uint32)size_bytes;
        pass.UploadBuffer(src, dst_r, false);
        pass.End();
        md::GpuDevice::Get().Submit(cmd);
    }
#endif
}

void SSBO::Bind(int binding_point) {
    (void)binding_point;
}

void SSBO::Shutdown() {
#ifdef MD_SDL_GPU
    md::GpuDeviceHandle dev = md::GpuDevice::Get().SDLDevice();
    for (int s = 0; s < 3; ++s) {
        if (sdl_transfer_[s]) { GpuReleaseTransferBuffer(dev, sdl_transfer_[s]); sdl_transfer_[s] = nullptr; }
        if (sdl_buf_[s])      { SDL_ReleaseGPUBuffer        (dev, sdl_buf_[s]);      sdl_buf_[s]      = nullptr; }
    }
    sdl_cap_ = 0;
#endif
}

#ifdef MD_SDL_GPU
SDL_GPUBuffer* SSBO::SDLBuffer() const {
    return sdl_buf_[md::GpuDevice::Get().FrameSlot()];
}

void SSBO::UploadInCmd(md::GpuCommandBufferHandle cmd, const void* data, int size_bytes, int offset) {
    int s = md::GpuDevice::Get().FrameSlot();
    if (!cmd || !sdl_buf_[s] || !sdl_transfer_[s] || !data || size_bytes <= 0) return;
    void* map = GpuMapTransfer(sdl_transfer_[s], false);
    if (map) {
        memcpy((uint8_t*)map + offset, data, (size_t)size_bytes);
        GpuUnmapTransfer(sdl_transfer_[s]);
    }
    GpuCopyPass pass;
    pass.Begin(cmd);
    SDL_GPUTransferBufferLocation src = {};
    src.transfer_buffer = sdl_transfer_[s];
    src.offset          = (Uint32)offset;
    SDL_GPUBufferRegion dst_r = {};
    dst_r.buffer = sdl_buf_[s];
    dst_r.offset = (Uint32)offset;
    dst_r.size   = (Uint32)size_bytes;
    pass.UploadBuffer(src, dst_r, false);
    pass.End();
}
#endif

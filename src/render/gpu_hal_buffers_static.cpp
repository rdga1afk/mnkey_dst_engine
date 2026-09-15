#include "gpu_hal_buffers_internal.h"
#include <monkey_dust/render/gpu_hal_free_functions.h>

// ── GpuStaticBuffer ───────────────────────────────────────────────────────────

void GpuStaticBuffer::Init(unsigned int target, const void* data, uint32_t size) {
#ifdef MD_SDL_GPU
    SDL_GPUDevice* dev = md::GpuDevice::Get().SDLDevice();

    // Map GL target → SDL_GPU buffer usage.
    // 0x8893 = GL_ELEMENT_ARRAY_BUFFER value (avoid GL header dependency in SDL-only builds).
    SDL_GPUBufferUsageFlags usage = (target == 0x8893u)  ? SDL_GPU_BUFFERUSAGE_INDEX
                                   : (target == GPU_TARGET_STORAGE) ? SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ
                                   : (target == GPU_TARGET_COMPUTE_STORAGE_RO) ? SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ
                                   : (target == GPU_TARGET_COMPUTE_STORAGE_RW) ? (SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ | SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_WRITE)
                                   : SDL_GPU_BUFFERUSAGE_VERTEX;
    (void)target;

    SDL_GPUBufferCreateInfo buf_info = {};
    buf_info.usage = usage;
    buf_info.size  = size;
    sdl_buf_ = SDL_CreateGPUBuffer(dev, &buf_info);
    if (!sdl_buf_) {
        MD_LOG(MD_LOG_WARNING, "[GpuStaticBuffer] SDL_CreateGPUBuffer failed: %s", SDL_GetError());
        return;
    }
    md::GpuResourceTracker::Get().OnBufferCreate();

    // One-shot upload: staging transfer buffer → device buffer.
    SDL_GPUTransferBufferCreateInfo tbuf_info = {};
    tbuf_info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    tbuf_info.size  = size;
    SDL_GPUTransferBuffer* transfer = SDL_CreateGPUTransferBuffer(dev, &tbuf_info);
    if (!transfer) {
        MD_LOG(MD_LOG_WARNING, "[GpuStaticBuffer] SDL_CreateGPUTransferBuffer failed: %s", SDL_GetError());
        return;
    }

    void* map = GpuMapTransfer(transfer, false);
    if (map) { memcpy(map, data, size); GpuUnmapTransfer(transfer); }

    SDL_GPUCommandBuffer* cmd  = SDL_AcquireGPUCommandBuffer(dev);
    if (!cmd) { MD_LOG(MD_LOG_WARNING, "[GpuStaticBuffer] AcquireCmd failed: %s", SDL_GetError()); return; }
    SDL_GPUCopyPass*      pass = SDL_BeginGPUCopyPass(cmd);
    SDL_GPUTransferBufferLocation src = {};
    src.transfer_buffer = transfer;
    src.offset          = 0;
    SDL_GPUBufferRegion dst = {};
    dst.buffer = sdl_buf_;
    dst.offset = 0;
    dst.size   = size;
    SDL_UploadToGPUBuffer(pass, &src, &dst, false /*no cycle — one shot*/);
    SDL_EndGPUCopyPass(pass);
    if (!SDL_SubmitGPUCommandBuffer(cmd))
        MD_LOG(MD_LOG_WARNING, "[GpuStaticBuffer] submit failed: %s", SDL_GetError());

    SDL_ReleaseGPUTransferBuffer(dev, transfer); // staging no longer needed
#endif

}

void GpuStaticBuffer::InitEmpty(unsigned int target, uint32_t size) {
#ifdef MD_SDL_GPU
    if (size == 0) { sdl_buf_ = nullptr; return; } // SDL_CreateGPUBuffer asserts on size<4 (audit S1-0b)
    SDL_GPUDevice* dev = md::GpuDevice::Get().SDLDevice();
    SDL_GPUBufferUsageFlags usage = (target == 0x8893u)  ? SDL_GPU_BUFFERUSAGE_INDEX
                                   : (target == GPU_TARGET_STORAGE) ? SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ
                                   : (target == GPU_TARGET_COMPUTE_STORAGE_RO) ? SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ
                                   : (target == GPU_TARGET_COMPUTE_STORAGE_RW) ? (SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ | SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_WRITE)
                                   : SDL_GPU_BUFFERUSAGE_VERTEX;
    SDL_GPUBufferCreateInfo buf_info = {};
    buf_info.usage = usage;
    buf_info.size  = size;
    sdl_buf_ = SDL_CreateGPUBuffer(dev, &buf_info);
    if (!sdl_buf_)
        MD_LOG(MD_LOG_WARNING, "[GpuStaticBuffer] InitEmpty: SDL_CreateGPUBuffer failed: %s", SDL_GetError());
    else
        md::GpuResourceTracker::Get().OnBufferCreate();
#else
    (void)target; (void)size;
#endif
}

bool GpuUploadBatch::Begin(uint32_t total_bytes) {
    cursor_ = 0; total_bytes_ = total_bytes; item_count_ = 0;
#ifdef MD_SDL_GPU
    transfer_ = nullptr; map_ = nullptr;
    if (total_bytes == 0) return true;
    SDL_GPUDevice* dev = md::GpuDevice::Get().SDLDevice();
    SDL_GPUTransferBufferCreateInfo tbuf_info = {};
    tbuf_info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    tbuf_info.size  = total_bytes;
    transfer_ = SDL_CreateGPUTransferBuffer(dev, &tbuf_info);
    if (!transfer_) {
        MD_LOG(MD_LOG_WARNING, "[GpuUploadBatch] SDL_CreateGPUTransferBuffer failed: %s", SDL_GetError());
        return false;
    }
    map_ = (uint8_t*)SDL_MapGPUTransferBuffer(dev, transfer_, false);
    if (!map_) {
        MD_LOG(MD_LOG_WARNING, "[GpuUploadBatch] SDL_MapGPUTransferBuffer failed: %s", SDL_GetError());
        SDL_ReleaseGPUTransferBuffer(dev, transfer_);
        transfer_ = nullptr;
        return false;
    }
    return true;
#else
    return true;
#endif
}

void GpuUploadBatch::Add(GpuStaticBuffer& buf, unsigned int target, const void* data, uint32_t size) {
    buf.InitEmpty(target, size);
#ifdef MD_SDL_GPU
    if (!map_ || item_count_ >= MAX_ITEMS || cursor_ + size > total_bytes_) return;
    if (data && size) memcpy(map_ + cursor_, data, size);
    items_[item_count_++] = { buf.SDLBuffer(), cursor_, size };
    cursor_ += size;
#else
    (void)buf; (void)target; (void)data; (void)size;
#endif
}

void GpuUploadBatch::End() {
#ifdef MD_SDL_GPU
    if (!transfer_) return;
    SDL_GPUDevice* dev = md::GpuDevice::Get().SDLDevice();
    SDL_UnmapGPUTransferBuffer(dev, transfer_);

    SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(dev);
    if (!cmd) {
        MD_LOG(MD_LOG_WARNING, "[GpuUploadBatch] AcquireCmd failed: %s", SDL_GetError());
        SDL_ReleaseGPUTransferBuffer(dev, transfer_);
        transfer_ = nullptr;
        return;
    }
    SDL_GPUCopyPass* pass = SDL_BeginGPUCopyPass(cmd);
    for (int i = 0; i < item_count_; ++i) {
        if (!items_[i].dst || items_[i].size == 0) continue;
        SDL_GPUTransferBufferLocation src = {};
        src.transfer_buffer = transfer_;
        src.offset          = items_[i].offset;
        SDL_GPUBufferRegion dst = {};
        dst.buffer = items_[i].dst;
        dst.offset = 0;
        dst.size   = items_[i].size;
        SDL_UploadToGPUBuffer(pass, &src, &dst, false);
    }
    SDL_EndGPUCopyPass(pass);
    if (!SDL_SubmitGPUCommandBuffer(cmd))
        MD_LOG(MD_LOG_WARNING, "[GpuUploadBatch] submit failed: %s", SDL_GetError());

    SDL_ReleaseGPUTransferBuffer(dev, transfer_);
    transfer_ = nullptr;
#endif
}

void GpuStaticBuffer::Shutdown() {
#ifdef MD_SDL_GPU
    if (sdl_buf_) {
        SDL_ReleaseGPUBuffer(md::GpuDevice::Get().SDLDevice(), sdl_buf_);
        sdl_buf_ = nullptr;
        md::GpuResourceTracker::Get().OnBufferDestroy();
    }
#endif
}

void GpuStaticBuffer::Bind(unsigned int target) const {
    (void)target;
}

void GpuStaticBuffer::BindVertex(uint32_t slot, uint32_t stride, uint64_t offset) const {
    (void)slot; (void)stride; (void)offset;
}

#include "gpu_hal_buffers_internal.h"
#include <monkey_dust/render/gpu_hal_free_functions.h>

// ── GpuStaticBuffer ───────────────────────────────────────────────────────────

// Upload batch (#119): between BeginUploadBatch and EndUploadBatch every Init records its copy into one command buffer / copy pass, submitted once,
// and stages its bytes in one persistent 4 MB transfer buffer (mapped with cycle once per batch; a 16 MB one made every cycle cost ~9 ms). A submit and a transfer buffer per Init cost 1.4-15 ms each
// while the GPU was busy with the frame. The buffers are valid for drawing after EndUploadBatch.
namespace {
SDL_GPUCommandBuffer*  s_batch_cmd = nullptr;
SDL_GPUCopyPass*       s_batch_pass = nullptr;
constexpr uint32_t     STAGE_CAP = 4u << 20;
SDL_GPUTransferBuffer* s_stage = nullptr;
uint8_t*               s_stage_map = nullptr;
uint32_t               s_stage_off = 0;
void FlushBatch(bool keep_open) {
    SDL_GPUDevice* dev = md::GpuDevice::Get().SDLDevice();
    if (s_stage_map) { GpuUnmapTransfer(s_stage); s_stage_map = nullptr; }
    s_stage_off = 0;
    if (s_batch_pass) { SDL_EndGPUCopyPass(s_batch_pass); s_batch_pass = nullptr; }
    if (s_batch_cmd) {
        if (!SDL_SubmitGPUCommandBuffer(s_batch_cmd)) MD_LOG(MD_LOG_WARNING, "[GpuStaticBuffer] batch submit failed: %s", SDL_GetError());
        s_batch_cmd = nullptr;
    }
    if (keep_open) {
        s_batch_cmd = SDL_AcquireGPUCommandBuffer(dev);
        if (s_batch_cmd) s_batch_pass = SDL_BeginGPUCopyPass(s_batch_cmd);
    }
}
}

void GpuStaticBuffer::BeginUploadBatch() {
    if (s_batch_cmd) return;
    SDL_GPUDevice* dev = md::GpuDevice::Get().SDLDevice();
    if (!s_stage) {
        SDL_GPUTransferBufferCreateInfo ti = {}; ti.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD; ti.size = STAGE_CAP;
        s_stage = SDL_CreateGPUTransferBuffer(dev, &ti);
        if (!s_stage) { MD_LOG(MD_LOG_WARNING, "[GpuStaticBuffer] staging buffer failed: %s", SDL_GetError()); return; }
    }
    s_batch_cmd = SDL_AcquireGPUCommandBuffer(dev);
    if (s_batch_cmd) s_batch_pass = SDL_BeginGPUCopyPass(s_batch_cmd);
}

void GpuStaticBuffer::EndUploadBatch() { FlushBatch(false); }

void GpuStaticBuffer::Init(unsigned int target, const void* data, uint32_t size) {
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

    if (s_batch_pass && s_stage && size <= STAGE_CAP) {   // batched: stage into the shared transfer buffer, copy in EndUploadBatch
        uint32_t off = (s_stage_off + 15u) & ~15u;
        if (off + size > STAGE_CAP) { FlushBatch(true); off = 0; }
        if (s_batch_pass) {
            if (!s_stage_map) s_stage_map = (uint8_t*)GpuMapTransfer(s_stage, true);
            if (s_stage_map) {
                memcpy(s_stage_map + off, data, size);
                SDL_GPUTransferBufferLocation bsrc = {}; bsrc.transfer_buffer = s_stage; bsrc.offset = off;
                SDL_GPUBufferRegion bdst = {}; bdst.buffer = sdl_buf_; bdst.size = size;
                SDL_UploadToGPUBuffer(s_batch_pass, &bsrc, &bdst, false);
                s_stage_off = off + size;
                return;
            }
        }
    }

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

}

void GpuStaticBuffer::Shutdown() {
    if (sdl_buf_) {
        SDL_ReleaseGPUBuffer(md::GpuDevice::Get().SDLDevice(), sdl_buf_);
        sdl_buf_ = nullptr;
        md::GpuResourceTracker::Get().OnBufferDestroy();
    }
}

void GpuStaticBuffer::Bind(unsigned int target) const {
    (void)target;
}


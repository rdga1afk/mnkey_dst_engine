#include <monkey_dust/render/gpu_ring_buffer.h>

#include <monkey_dust/platform/md_log.h>

#ifdef MD_SDL_GPU
#include <monkey_dust/render/gpu_device.h>
#include <monkey_dust/render/gpu_copy_pass.h>
#endif


// ── Init / Shutdown ────────────────────────────────────────────────────────────

void GpuRingBuffer::Init(uint32_t size_bytes, int binding_hint) {
    size_ = size_bytes;
    cur_  = 0;

#ifdef MD_SDL_GPU
    sdl_size_ = size_bytes;
    md::GpuDeviceHandle dev = md::GpuDevice::Get().SDLDevice();
    if (dev && size_bytes > 0) {
        SDL_GPUBufferCreateInfo bi = {};
        bi.usage = SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ;
        bi.size  = size_bytes;

        SDL_GPUTransferBufferCreateInfo ti = {};
        ti.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
        ti.size  = size_bytes;

        for (int s = 0; s < N_FRAMES; ++s) {
            sdl_device_[s] = SDL_CreateGPUBuffer(dev, &bi);
            if (!sdl_device_[s])
                MD_LOG(MD_LOG_WARNING, "[GpuRingBuffer] SDL_CreateGPUBuffer[%d] failed: %s", s, SDL_GetError());

            sdl_staging_[s] = SDL_CreateGPUTransferBuffer(dev, &ti);
            if (!sdl_staging_[s])
                MD_LOG(MD_LOG_WARNING, "[GpuRingBuffer] SDL_CreateGPUTransferBuffer[%d] failed: %s", s, SDL_GetError());
        }
    }
#endif

    (void)binding_hint;
}

void GpuRingBuffer::Shutdown() {
#ifdef MD_SDL_GPU
    md::GpuDeviceHandle dev = md::GpuDevice::Get().SDLDevice();
    for (int s = 0; s < N_FRAMES; ++s) {
        if (sdl_device_[s])  { SDL_ReleaseGPUBuffer        (dev, sdl_device_[s]);  sdl_device_[s]  = nullptr; }
        if (sdl_staging_[s]) { SDL_ReleaseGPUTransferBuffer(dev, sdl_staging_[s]); sdl_staging_[s] = nullptr; }
    }
    sdl_size_ = 0;
#endif
    size_ = 0;
    cur_  = 0;
}

// ── MapWrite / Unmap / BindStorage / Advance — OpenGL ring path ───────────────

void* GpuRingBuffer::MapWrite() {
    return nullptr;
}

void GpuRingBuffer::Unmap() {
}

void GpuRingBuffer::BindStorage(int slot) {
    (void)slot;
}

void GpuRingBuffer::Advance() {
    // SDL_GPU: slot managed by GpuDevice::FrameSlot(). No-op here.
}


// ── SDL_GPU staging path ───────────────────────────────────────────────────────

#ifdef MD_SDL_GPU
SDL_GPUBuffer* GpuRingBuffer::SDLBuffer() const {
    return sdl_device_[md::GpuDevice::Get().FrameSlot()];
}

void* GpuRingBuffer::MapWriteSDL() {
    int s = md::GpuDevice::Get().FrameSlot();
    if (!sdl_staging_[s]) return nullptr;
    return SDL_MapGPUTransferBuffer(md::GpuDevice::Get().SDLDevice(), sdl_staging_[s], false);
}

void GpuRingBuffer::UnmapSDL() {
    int s = md::GpuDevice::Get().FrameSlot();
    if (sdl_staging_[s])
        SDL_UnmapGPUTransferBuffer(md::GpuDevice::Get().SDLDevice(), sdl_staging_[s]);
}

void GpuRingBuffer::Upload(md::GpuCommandBufferHandle cmd) {
    int s = md::GpuDevice::Get().FrameSlot();
    if (!cmd || !sdl_device_[s] || !sdl_staging_[s]) return;
    GpuCopyPass cp;
    cp.Begin(cmd);
    UploadInPass(cp.SDLPass());
    cp.End();
}

// iGPU pattern: caller opens a shared copy pass across multiple ring buffers.
// Reduces copy-pass begin/end overhead from N calls to 1 per frame.
void GpuRingBuffer::UploadInPass(SDL_GPUCopyPass* pass) {
    int s = md::GpuDevice::Get().FrameSlot();
    if (!pass || !sdl_device_[s] || !sdl_staging_[s]) return;
    SDL_GPUTransferBufferLocation src = {};
    src.transfer_buffer = sdl_staging_[s];
    src.offset          = 0;
    SDL_GPUBufferRegion dst = {};
    dst.buffer = sdl_device_[s];
    dst.offset = 0;
    dst.size   = sdl_size_;
    GpuCopyPass::FromRaw(pass, nullptr).UploadBuffer(src, dst, false);
}
#endif

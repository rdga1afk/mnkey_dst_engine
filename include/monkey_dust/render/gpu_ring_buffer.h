#pragma once
#include <cstdint>

#include <SDL3/SDL_gpu.h>
#include <monkey_dust/render/gpu_device.h>

// GpuRingBuffer — per-frame CPU→GPU data upload with ring-buffering.
//
// OpenGL path (GL 4.4+):
//   N_FRAMES × size_bytes persistent-coherent SSBO.
//   MapWrite() returns a CPU pointer to the current frame's sub-range.
//   Advance() inserts a fence + rotates slot.
//
// SDL_GPU path:
//   sdl_staging_ (SDL_GPUTransferBuffer, cycle=true) + sdl_device_ (SDL_GPUBuffer).
//   MapWriteSDL() maps staging; UnmapSDL() unmaps; Upload(cmd) copies to device.
//   Advance() is a no-op — cycle=true handles internal versioning.
//
// Dual-backend: both paths coexist. MapWrite() returns the GL coherent pointer.
// Use MapWriteSDL()/UnmapSDL()/Upload(cmd) for the SDL_GPU device buffer.
class GpuRingBuffer {
public:
    static constexpr int N_FRAMES = 3;

    void Init(uint32_t size_bytes, int binding_hint = -1);
    void Shutdown();

    // OpenGL: persistent-coherent CPU write pointer. SDL_GPU-only: returns nullptr.
    void* MapWrite();
    // No-op for coherent GL mapping. SDL_GPU-only: no-op. Use UnmapSDL() for SDL_GPU.
    void  Unmap();
    // OpenGL: glBindBufferRange SSBO at binding slot. No-op in SDL_GPU-only.
    void  BindStorage(int slot);
    // OpenGL: insert GL fence + rotate ring slot. SDL_GPU-only: no-op.
    void  Advance();

    uint32_t size_bytes() const { return size_; }
    int      current()    const { return cur_; }


    // Map the current frame's staging buffer for CPU writing.
    // Follow with UnmapSDL() then Upload(cmd) before any compute pass reads this buffer.
    void*          MapWriteSDL();
    void           UnmapSDL();
    // Record a copy pass in cmd: current staging → current device buffer.
    void           Upload(md::GpuCommandBufferHandle cmd);
    // iGPU zero-copy adaptation: upload within a caller-owned copy pass.
    // Use when batching multiple ring buffer uploads in a single copy pass
    // to reduce driver overhead (N copy-pass begin/end → 1 per frame).
    void           UploadInPass(SDL_GPUCopyPass* pass);
    // Returns the current frame's device buffer (slot = GpuDevice::FrameSlot()).
    SDL_GPUBuffer* SDLBuffer() const;

private:
    SDL_GPUBuffer*         sdl_device_[3]  = {};
    SDL_GPUTransferBuffer* sdl_staging_[3] = {};
    uint32_t               sdl_size_       = 0;
    uint32_t size_ = 0;
    int      cur_  = 0;
};

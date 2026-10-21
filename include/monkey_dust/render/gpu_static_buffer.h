#pragma once
// gpu_static_buffer.h — GpuStaticBuffer (split from
// gpu_hal.h, code audit proposal #8).

#include <cstdint>

#include <SDL3/SDL_gpu.h>

// Sentinel gl_target value for GpuStaticBuffer::Init — requests
// SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ instead of VERTEX/INDEX, so the
// resulting buffer can be bound via SDL_BindGPUFragmentStorageBuffers (a
// StructuredBuffer<T> in shader code). Doesn't collide with any real GL
// buffer-target enum (those are all small, e.g. GL_ARRAY_BUFFER=0x8892).
static constexpr unsigned int GPU_TARGET_STORAGE = 0xFFFFFFFEu;
// M1 HAL-closure, §3 item 8 (2026-08-31): real one-shot buffer-creation call
// sites (npc_gpu_culler.cpp's pos_buf_/vis_buf_, terrain_vt_page_cache.cpp's
// page_meta_buf_) all need COMPUTE_STORAGE_READ and/or _WRITE usage, which
// GPU_TARGET_STORAGE's GRAPHICS_STORAGE_READ mapping cannot express -- a
// real usage-taxonomy gap, not a bypass to shame (this class was built for
// vertex/index/graphics-storage-read buffers; compute storage never had a
// caller until now). Two sentinels, matching the two real usage shapes
// found (no _WO/write-only compute variant exists as a real need yet).
static constexpr unsigned int GPU_TARGET_COMPUTE_STORAGE_RO = 0xFFFFFFFDu;
static constexpr unsigned int GPU_TARGET_COMPUTE_STORAGE_RW = 0xFFFFFFFCu;

class GpuStaticBuffer {
public:
    // gl_target: GL_ARRAY_BUFFER or GL_ELEMENT_ARRAY_BUFFER (used for usage hint),
    // GPU_TARGET_STORAGE for a fragment-storage-buffer-bindable resource, or
    // GPU_TARGET_COMPUTE_STORAGE_RO/_RW for a compute-storage-buffer-bindable
    // resource (read-only or read-write). SDL_GPU path maps
    // GL_ELEMENT_ARRAY_BUFFER → INDEX usage, GPU_TARGET_STORAGE →
    // GRAPHICS_STORAGE_READ, GPU_TARGET_COMPUTE_STORAGE_RO →
    // COMPUTE_STORAGE_READ, GPU_TARGET_COMPUTE_STORAGE_RW →
    // COMPUTE_STORAGE_READ|COMPUTE_STORAGE_WRITE, rest → VERTEX.
    void Init(unsigned int gl_target, const void* data, uint32_t size_bytes);
    void Shutdown();

    // Upload batch: Init calls between Begin and End share one command buffer and one submit (load-time streaming, #119). Not for use while a render pass is open.
    static void BeginUploadBatch();
    static void EndUploadBatch();

    // OpenGL bind helpers (no-op in SDL_GPU-only builds).
    void Bind(unsigned int gl_target) const;

    unsigned int GLBuffer() const { return gl_buf_; }
    SDL_GPUBuffer* SDLBuffer() const { return sdl_buf_; }

    // Transfer GL buffer ownership (for MdMesh legacy interop).
    unsigned int Release() { unsigned int id = gl_buf_; gl_buf_ = 0; return id; }

private:
    unsigned int gl_buf_ = 0;
    SDL_GPUBuffer* sdl_buf_ = nullptr;
};

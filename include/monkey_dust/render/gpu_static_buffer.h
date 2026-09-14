#pragma once
// gpu_static_buffer.h — GpuStaticBuffer / GpuUploadBatch (split from
// gpu_hal.h, code audit proposal #8).

#include <cstdint>

#ifdef MD_SDL_GPU
#include <SDL3/SDL_gpu.h>
#endif

// Sentinel gl_target value for GpuStaticBuffer::Init/InitEmpty — requests
// SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ instead of VERTEX/INDEX, so the
// resulting buffer can be bound via SDL_BindGPUFragmentStorageBuffers (a
// StructuredBuffer<T> in shader code). Doesn't collide with any real GL
// buffer-target enum (those are all small, e.g. GL_ARRAY_BUFFER=0x8892).
// Lets read-only per-chunk data (e.g. terrain_chunk.h's steepness_ssbo) ride
// the SAME GpuUploadBatch used for vbo/ibo/skirt — one shared submit instead
// of a second per-chunk synchronous upload (see GpuUploadBatch doc comment
// for why that matters on Intel ANV).
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
    // Creates the GPU-side buffer only, no data upload — pair with GpuUploadBatch::Add()
    // (gpu_hal.h below) to fill it. Use when uploading many small buffers at once
    // (e.g. per-chunk terrain VBO/IBO/skirt/LOD) — Init()'s own per-call transfer
    // buffer + command buffer submit does not scale to thousands of calls (see
    // GpuUploadBatch doc comment).
    void InitEmpty(unsigned int gl_target, uint32_t size_bytes);
    void Shutdown();

    // OpenGL bind helpers (no-op in SDL_GPU-only builds).
    void Bind(unsigned int gl_target) const;
    void BindVertex(uint32_t slot, uint32_t stride, uint64_t offset = 0) const;

    unsigned int GLBuffer() const { return gl_buf_; }
#ifdef MD_SDL_GPU
    SDL_GPUBuffer* SDLBuffer() const { return sdl_buf_; }
#endif

    // Transfer GL buffer ownership (for MdMesh legacy interop).
    unsigned int Release() { unsigned int id = gl_buf_; gl_buf_ = 0; return id; }

private:
    unsigned int gl_buf_ = 0;
#ifdef MD_SDL_GPU
    SDL_GPUBuffer* sdl_buf_ = nullptr;
#endif
};

// ─────────────────────────────────────────────────────────────────────────────
// GpuUploadBatch — merges N GpuStaticBuffer uploads into ONE transfer buffer +
// ONE command buffer submit, instead of GpuStaticBuffer::Init's one-of-each
// per call. Needed whenever a loop uploads many small buffers back to back
// (e.g. a chunk's vbo+ibo+3×ibo_lod+skirt_vbo+skirt_ibo = 7 buffers, repeated
// per chunk): 4096 chunks × 7 unbatched Init() calls (each its own
// CreateGPUTransferBuffer + AcquireCommandBuffer + Submit) was enough one-shot
// command-buffer churn to corrupt Intel ANV driver-internal state and abort
// mid-vkAllocateCommandBuffers (observed: editor's full 64×64 world 3D View
// tab, "malloc(): corrupted top size" right as the last chunks finish).
// Usage: Begin(total_bytes) → Add() once per buffer (Add() creates the GPU-side
// buffer via InitEmpty and copies into the batch's mapped region) → End()
// (one copy pass, one submit). MAX_ITEMS fixed array — no heap allocation.
class GpuUploadBatch {
public:
    static constexpr int MAX_ITEMS = 16;

    bool Begin(uint32_t total_bytes);
    void Add(GpuStaticBuffer& buf, unsigned int gl_target, const void* data, uint32_t size_bytes);
    void End();

private:
#ifdef MD_SDL_GPU
    SDL_GPUTransferBuffer* transfer_ = nullptr;
    uint8_t*               map_      = nullptr;
    struct Item { SDL_GPUBuffer* dst; uint32_t offset; uint32_t size; };
    Item items_[MAX_ITEMS];
#endif
    uint32_t cursor_      = 0;
    uint32_t total_bytes_ = 0;
    int      item_count_  = 0;
};

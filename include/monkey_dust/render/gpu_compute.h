#pragma once
// gpu_compute.h — GpuComputePipeline / GpuComputeStorageBindings /
// GpuComputePass (split from gpu_hal.h, code audit proposal #8).

#include <cstdint>

#ifdef MD_SDL_GPU
#include <SDL3/SDL_gpu.h>
#endif

// ─────────────────────────────────────────────────────────────────────────────
// GpuComputePipeline — immutable compute shader program.
// SDL_GPU: SDL_CreateGPUComputePipeline (SPIR-V binary; see shaders/spirv/)
// ─────────────────────────────────────────────────────────────────────────────
class GpuComputePipeline {
public:
    struct Desc {
        const char* glsl_path = nullptr; // OpenGL GLSL source; SPIR-V path derived by MakeSpvPath
        // SDL_GPU resource counts — must match SPIR-V declarations exactly.
        uint32_t num_uniform_buffers            = 0;
        uint32_t num_readonly_storage_buffers   = 0;
        uint32_t num_readwrite_storage_buffers  = 0;
        uint32_t num_readonly_storage_textures  = 0;
        uint32_t num_readwrite_storage_textures = 0;
        uint32_t num_samplers                   = 0;
        // Must match layout(local_size_x/y/z) in the compute shader.
        uint32_t threadcount_x = 64;
        uint32_t threadcount_y = 1;
        uint32_t threadcount_z = 1;
    };

    bool Create(const Desc& desc);
    void Destroy();
    // OpenGL: glGetUniformLocation. SDL_GPU: always -1 (use PushUniforms instead).
    int  UniformLoc(const char* name) const;

#ifdef MD_SDL_GPU
    SDL_GPUComputePipeline* SDLComputePipeline() const { return sdl_pipeline_; }
#endif

private:
    friend class GpuComputePass;
    unsigned int program_ = 0;
#ifdef MD_SDL_GPU
    SDL_GPUComputePipeline* sdl_pipeline_ = nullptr;
#endif
};

// ─────────────────────────────────────────────────────────────────────────────
// GpuComputePass::StorageBindings — declared outside the class so it can be
// used as a default argument (GCC rejects nested-struct DMIs in that context).
// ─────────────────────────────────────────────────────────────────────────────
struct GpuComputeStorageBindings {
#ifdef MD_SDL_GPU
    SDL_GPUCommandBuffer* cmd = nullptr;
    // Read-write storage buffers — declared to SDL_BeginGPUComputePass.
    // Maps slot index to SPIR-V set=1 read-write bindings in ascending binding order.
    SDL_GPUStorageBufferReadWriteBinding rw_buffers[8] = {};
    uint32_t                             num_rw_buffers = 0;
    // Read-only storage buffers — bound via SDL_BindGPUComputeStorageBuffers.
    SDL_GPUBuffer* ro_buffers[8] = {};
    uint32_t       num_ro_buffers = 0;
    // Read-write storage TEXTURES — also declared to SDL_BeginGPUComputePass
    // (its 2nd/3rd args, previously hardcoded nullptr,0 — see GpuComputePass::
    // Begin's doc comment for the 3 real sites this unblocks: terrain normal
    // bake, terrain page-fill, SSAOSystem::Dispatch, all writing a storage
    // texture, not just buffers).
    SDL_GPUStorageTextureReadWriteBinding rw_textures[4] = {};
    uint32_t                              num_rw_textures = 0;
#endif
};

// ─────────────────────────────────────────────────────────────────────────────
// GpuComputePass — scoped compute dispatch with explicit memory barrier.
//
// OpenGL usage:
//   pass.Begin(&pipeline);        // bind compute program
//   pass.SetUniform*(...);        // set uniforms via GL locations
//   pass.Dispatch(gx, gy, gz);
//   pass.End(BARRIER_STORAGE);    // glMemoryBarrier + unbind
//
// SDL_GPU usage:
//   GpuComputeStorageBindings b; b.cmd = cmd; b.rw_buffers[0] = {sdl_buf, false}; b.num_rw_buffers = 1;
//   pass.Begin(&pipeline, b);     // SDL_BeginGPUComputePass + SDL_BindGPUComputePipeline
//   pass.PushUniforms(0, &data, sizeof(data));
//   pass.Dispatch(gx, gy, gz);    // SDL_DispatchGPUCompute
//   pass.End();                   // SDL_EndGPUComputePass (barriers implicit)
//
// Storage-texture note (fixed as part of the M1 copy/upload/download group):
// Begin used to hardcode SDL_BeginGPUComputePass's storage-texture args to
// nullptr,0, forcing any dispatch that writes a storage TEXTURE (not just
// buffers) to bypass this class entirely and hand-roll raw SDL — 3 confirmed
// real sites did exactly that (terrain_world_heightmap.cpp's normal bake,
// terrain_vt_page_cache.cpp's page-fill, SSAOSystem::Dispatch's AO write).
// A repo-wide grep for functions taking a raw SDL_GPUComputePass* parameter
// found none — unlike GpuCopyPass, no real caller-shares-a-compute-pass
// scenario exists, so there is no FromRaw here; the actual gap was purely
// the storage-texture hardcode plus a missing sampler-bind method, both
// fixed below instead of adding an unneeded non-owning variant.
// ─────────────────────────────────────────────────────────────────────────────
class GpuComputePass {
public:
    static constexpr uint32_t BARRIER_STORAGE         = 1u; // GL_SHADER_STORAGE_BARRIER_BIT
    static constexpr uint32_t BARRIER_COMMAND         = 2u; // GL_COMMAND_BARRIER_BIT
    static constexpr uint32_t BARRIER_STORAGE_COMMAND = 3u; // both — for indirect draw output

    using StorageBindings = GpuComputeStorageBindings; // backward-compat alias

    // Bind pipeline and (SDL_GPU) open compute pass.
    // bindings is required for SDL_GPU; ignored in OpenGL.
    void Begin(GpuComputePipeline* pipeline, const StorageBindings& bindings = StorageBindings{});

#ifdef MD_SDL_GPU
    // Push uniform data (UBO slot) for the compute shader.
    void PushUniforms(uint32_t slot, const void* data, uint32_t size_bytes);
    // Bind texture-sampler pairs (set=0, ascending binding order) for the
    // compute shader — the 3 storage-texture sites above all need this;
    // GpuComputePass had no equivalent before (only ro_buffers went through
    // Begin's StorageBindings; textures had no path in at all).
    void BindSamplers(uint32_t first_slot, const SDL_GPUTextureSamplerBinding* bindings, uint32_t count);
#endif

    void Dispatch(uint32_t gx, uint32_t gy, uint32_t gz);

    // barrier_flags: BARRIER_* constants (OpenGL). SDL_GPU barriers are implicit.
    void End(uint32_t barrier_flags = BARRIER_STORAGE);

#ifdef MD_SDL_GPU
    // Null after a failed Begin (e.g. SDL_BeginGPUComputePass returned
    // nullptr) — callers that need to detect that failure (matching the
    // raw-SDL call sites this Begin extension replaced) check this.
    SDL_GPUComputePass* SDLPass() const { return sdl_pass_; }
#endif

private:
    GpuComputePipeline* pipeline_ = nullptr;
#ifdef MD_SDL_GPU
    SDL_GPUCommandBuffer* sdl_cmd_  = nullptr;
    SDL_GPUComputePass*   sdl_pass_ = nullptr;
#endif
};

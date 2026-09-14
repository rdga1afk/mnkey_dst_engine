#pragma once
// gpu_pipeline.h — GpuPipeline / GpuVertexBuffer / GpuCommandBuffer (split
// from gpu_hal.h, code audit proposal #8). Core graphics-pipeline creation,
// per-frame vertex staging, and render-pass recording.

#include <monkey_dust/render/gpu_hal_types.h>
#include <monkey_dust/render/md_shader.h>
#include <monkey_dust/render/gpu_ring_buffer.h>
#include <cstdint>

#ifdef MD_SDL_GPU
#include <SDL3/SDL_gpu.h>
#endif

// ─────────────────────────────────────────────────────────────────────────────
// GpuPipeline — immutable graphics pipeline.
// Create once at init; bind per draw call.
// SDL_GPU: SDL_CreateGPUGraphicsPipeline
// ─────────────────────────────────────────────────────────────────────────────
class GpuPipeline {
public:
    struct Desc {
        const char*     vert_path = nullptr;  // GLSL (OpenGL) or SPIR-V basename (SDL_GPU)
        const char*     frag_path = nullptr;
        uint32_t        shader_features = SF_None; // bitmask of ShaderFeature — selects .spv variant
        GpuVertexLayout layout;
        GpuRasterState  raster;
#ifdef MD_SDL_GPU
        // Shader resource counts required by SDL_GPUShaderCreateInfo.
        // Must match the SPIR-V descriptor declarations exactly.
        uint32_t vert_uniform_bufs = 0;
        uint32_t vert_samplers     = 0;
        uint32_t vert_storage_bufs = 0;
        uint32_t frag_uniform_bufs = 0;
        uint32_t frag_storage_bufs = 0;
        uint32_t frag_samplers     = 0;
        bool     has_depth_target  = true;  // set false for 2D/HUD passes
        bool     depth_only        = false; // depth-only pass (shadow): no color target
        // Override swapchain color format (e.g. SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM
        // for intermediate render targets). INVALID (0) = use swapchain format.
        SDL_GPUTextureFormat color_format = SDL_GPU_TEXTUREFORMAT_INVALID;
#endif
    };

    bool Create(const Desc& desc);
    void Destroy();
    // Re-read SPV from disk and recreate pipeline. Safe to call mid-frame on next frame boundary.
    bool Reload();

    // OpenGL: glGetUniformLocation. SDL_GPU: always returns -1 (use PushUniforms instead).
    int UniformLoc(const char* name) const;
#ifdef MD_SDL_GPU
    SDL_GPUGraphicsPipeline* SDLPipeline() const { return sdl_pipeline_; }
#endif

private:
    friend class GpuCommandBuffer;
#ifdef MD_SDL_GPU
    SDL_GPUGraphicsPipeline* sdl_pipeline_ = nullptr;
#endif
    GpuRasterState raster_ = {};
    Desc           desc_   = {};  // stored at Create() for Reload()
};

// ─────────────────────────────────────────────────────────────────────────────
// GpuVertexBuffer — per-frame vertex data with ring buffering.
// OpenGL: GpuRingBuffer (GL 4.4 persistent coherent mapping, N=3 fence slots).
// SDL_GPU: SDL_GPUTransferBuffer (CPU staging, cycle=true) +
//          SDL_GPUBuffer (device VERTEX, cycle=true upload).
// ─────────────────────────────────────────────────────────────────────────────
class GpuVertexBuffer {
public:
    void Init(uint32_t max_vertices, uint32_t vertex_stride);
    void Shutdown();

    // Get CPU-writable pointer for this frame's vertex data.
    // SDL_GPU: SDL_MapGPUTransferBuffer(cycle=true)
    void* MapWrite();

    // Flush — no-op for coherent GL mapping; SDL_UnmapGPUTransferBuffer.
    void  Unmap();

    // SDL_GPU: copy transfer → device buffer via copy pass inside cmd.
    // Must be called before the render pass that reads this buffer.
    // OpenGL: no-op (data already coherently visible).
#ifdef MD_SDL_GPU
    void Upload(SDL_GPUCommandBuffer* cmd);
    SDL_GPUBuffer* SDLBuffer() const { return sdl_buf_; }
#endif

    // Insert fence + rotate ring slot (OpenGL). No-op in SDL_GPU path.
    void Advance();

    uint32_t Stride() const { return stride_; }

private:
    friend class GpuCommandBuffer;
    GpuRingBuffer ring_;       // OpenGL path (no-op stub when !MD_OPENGL43_ENABLED)
    uint32_t      stride_ = 0;
#ifdef MD_SDL_GPU
    SDL_GPUBuffer*         sdl_buf_      = nullptr;
    SDL_GPUTransferBuffer* sdl_transfer_ = nullptr;
    uint32_t               sdl_size_     = 0;
#endif
};

// ─────────────────────────────────────────────────────────────────────────────
// GpuCommandBuffer — records (and immediately executes) one render pass.
// SDL_GPU: SDL_GPUCommandBuffer + SDL_BeginGPURenderPass
// ─────────────────────────────────────────────────────────────────────────────
class GpuCommandBuffer {
public:
    // Bind pipeline: apply shader + raster state.
    // SDL_GPU: SDL_BindGPUGraphicsPipeline
    void BindPipeline(GpuPipeline* pipeline);

    // Bind vertex data source.
    // SDL_GPU: SDL_BindGPUVertexBuffers
    void BindVertexBuffer(GpuVertexBuffer* buf);

    // Upload small per-draw constants via named uniform location.
    // SDL_GPU: no-op — use PushVertexUniforms / PushFragmentUniforms instead.
    void SetUniformMat4(int loc, const float* m16);
    void SetUniformVec3(int loc, const float* v3);

    // Issue draw call.
    // SDL_GPU: SDL_DrawGPUPrimitives
    void Draw(uint32_t vertex_count, uint32_t first_vertex = 0);

    // Restore GL state / end SDL_GPU render pass.
    void EndPass();

#ifdef MD_SDL_GPU
    // Open a color (+ optional depth) render pass on an existing command buffer.
    struct ColorPassDesc {
        // MRT: gbuffer.cpp is the only 2-target site in the whole tree
        // (confirmed by grep, M1 §3 item 12) -- sized to that, not grown
        // speculatively. color_tex[0] is the only slot every other caller
        // (28 sites) ever touches; num_color_targets defaults to 1 so none
        // of them change behavior.
        static constexpr int  MAX_COLOR_TARGETS = 2;
        SDL_GPUCommandBuffer* cmd;
        SDL_GPUTexture*       color_tex[MAX_COLOR_TARGETS] = {};
        int                   num_color_targets = 1;
        SDL_GPUTexture*       depth_tex      = nullptr;
        float                 clear_color[4] = {0.f, 0.f, 0.f, 1.f};
        // Separate field, not clear_color[MAX_COLOR_TARGETS][4]: gbuffer.cpp's
        // RT0/RT1 clear colors genuinely differ (confirmed by reading
        // GBuffer::Begin -- {0,0,0,1} vs {0,0,0,0}), so target 1 needs its own
        // clear value. Kept apart from clear_color so the 28 existing
        // single-target callers never have to touch a 2D array they don't use.
        // Only read when num_color_targets > 1.
        float                 clear_color_mrt1[4] = {0.f, 0.f, 0.f, 0.f};
        float                 clear_depth    = 1.0f;
        bool                  load_color     = false; // false = CLEAR on entry
        bool                  load_depth     = false;
        // 2026-08-30 (M1 "owns-its-pass" group 3): LOADOP_DONT_CARE on an
        // iGPU with a shared memory bus (Intel HD 520, 25.6 GB/s) is a real
        // bandwidth saving, not a micro-opt -- it tells the driver to skip
        // reading the previous render-target contents entirely, which
        // matters for fullscreen post-process passes that fully overwrite
        // their target every call. Replacing it with CLEAR would be a
        // silent behavior change that pixel-diff A/B cannot catch (CLEAR
        // produces the same visible result ahead of a full overwrite --
        // the only difference is memory traffic, which no A/B in this
        // repo measures). Both flags default to false = identical to the
        // pre-extension struct, so no existing caller (5 call sites as of
        // this comment) changes behavior by not setting them.
        bool                  color_dont_care     = false; // true overrides load_color -> LOADOP_DONT_CARE
        bool                  depth_discard_after = false; // true -> depth STOREOP_DONT_CARE (mirrors DepthDesc::discard_after)
    };
    void BeginColorPass(const ColorPassDesc& desc);

    // Bind textures + samplers for the fragment stage.
    void BindFragmentSamplers(uint32_t first_slot,
                               const SDL_GPUTextureSamplerBinding* bindings,
                               uint32_t count);

    // Push small uniform structs (UBO slot 0/1 mapped via SPIRV_COMPILE bindings).
    void PushVertexUniforms  (uint32_t slot, const void* data, uint32_t size_bytes);
    void PushFragmentUniforms(uint32_t slot, const void* data, uint32_t size_bytes);

    SDL_GPURenderPass*    SDLPass() const { return sdl_pass_; }
    SDL_GPUCommandBuffer* SDLCmd()  const { return sdl_cmd_; }
#endif

private:
    GpuPipeline* pipeline_ = nullptr;
#ifdef MD_SDL_GPU
    SDL_GPUCommandBuffer* sdl_cmd_  = nullptr;
    SDL_GPURenderPass*    sdl_pass_ = nullptr;
#endif
};

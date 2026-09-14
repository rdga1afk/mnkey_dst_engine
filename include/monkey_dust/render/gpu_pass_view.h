#pragma once
// gpu_pass_view.h — GpuPassView / GpuRenderPass (split from gpu_hal.h, code
// audit proposal #8). GpuRenderPass::View() returns a GpuPassView, so these
// two stay together despite GpuPassView being SDL_GPU-only and GpuRenderPass
// being dual-backend.

#include <monkey_dust/render/gpu_pipeline.h>
#include <monkey_dust/render/gpu_static_buffer.h>
#include <monkey_dust/render/gpu_depth_texture.h>
#include <cstdint>

#ifdef MD_SDL_GPU
#include <SDL3/SDL_gpu.h>
#endif

#ifdef MD_SDL_GPU
class GpuStaticBuffer; // defined below -- GpuPassView only needs pointer params

// ─────────────────────────────────────────────────────────────────────────────
// GpuPassView — non-owning handle for binding/drawing into an ALREADY-OPEN
// render pass. No Begin()/End(): lifetime is the call, not ownership.
//
// Why this exists (docs/HAL_CLOSURE_INVENTORY.md, M1 pilot): GpuCommandBuffer
// and GpuRenderPass both OWN their pass's lifecycle (BeginColorPass/BeginColor
// create sdl_pass_, End*() destroys it) -- neither can wrap a pass some OTHER
// caller already opened. But most scene-drawing code (billboard/prop/clutter/
// vegetation renderers) is called INTO a pass its caller opened once and
// shares across many draws for perf (one pass, not N) -- exactly the shape
// neither owning wrapper fits. That mismatch, not carelessness, is why ~90%
// of scene code bypassed the HAL before M1.
//
// Two entry points:
//   - GpuRenderPass::View(cmd) -- the pass already went through Begin*() on
//     a GpuRenderPass. This is the steady-state path once M1 finishes
//     migrating pass-openers off raw SDL_BeginGPURenderPass.
//   - GpuPassView::FromRaw(rp, cmd) -- TEMPORARY, remove after M1: for call
//     sites that still receive a raw SDL_GPURenderPass* from a caller not
//     yet migrated to GpuRenderPass. Do not add new FromRaw() call sites
//     once the caller side is migrated -- route through View() instead.
// ─────────────────────────────────────────────────────────────────────────────
class GpuPassView {
public:
    static GpuPassView FromRaw(SDL_GPURenderPass* rp, SDL_GPUCommandBuffer* cmd) {
        return GpuPassView(rp, cmd);
    }

    void BindPipeline(GpuPipeline* pipeline);
    void BindVertexBuffer(GpuVertexBuffer* buf);
    // Overload for the other vertex-buffer-shaped class in the HAL --
    // GpuStaticBuffer (e.g. terrain-chunk clutter_vbo) exposes the same
    // SDLBuffer() accessor as GpuVertexBuffer but is a different type.
    void BindVertexBuffer(const GpuStaticBuffer* buf);
    // No wrapper existed for indexed binding/draw anywhere in the HAL before
    // this (docs/HAL_CLOSURE_INVENTORY.md §2.3/§2.4) -- added here because a
    // GpuPassView caller needs both to close a file completely, not deferred
    // to a later group when doing so would leave the file half-migrated.
    void BindIndexBuffer(const GpuStaticBuffer* buf, SDL_GPUIndexElementSize elem_size);
    void BindFragmentSamplers(uint32_t first_slot,
                               const SDL_GPUTextureSamplerBinding* bindings,
                               uint32_t count);
    // Vertex-stage sampler counterpart -- editor_char_preview_runtime.cpp's
    // hair draw samples a bone-matrices texture from the vertex shader via
    // SDL_BindGPUVertexSamplers (distinct call from the fragment one above).
    void BindVertexSamplers(uint32_t first_slot,
                             const SDL_GPUTextureSamplerBinding* bindings,
                             uint32_t count);
    // No wrapper existed for fragment storage-buffer binding before this --
    // added for terrain_shading_projected.cpp's DrawShadingResolve(), which
    // binds vt.PageMetaSSBO() via SDL_BindGPUFragmentStorageBuffers (a
    // different bind call than the sampler one above, same pass-scoped shape).
    void BindFragmentStorageBuffers(uint32_t first_slot,
                                     SDL_GPUBuffer* const* storage_buffers,
                                     uint32_t count);
    // Same gap, vertex-stage side -- npc_render_frame_prep.cpp's depth
    // prepass binds TransformSoA/vis/faction/bone SSBOs to the vertex stage
    // via SDL_BindGPUVertexStorageBuffers.
    void BindVertexStorageBuffers(uint32_t first_slot,
                                   SDL_GPUBuffer* const* storage_buffers,
                                   uint32_t count);
    void PushVertexUniforms  (uint32_t slot, const void* data, uint32_t size_bytes);
    void PushFragmentUniforms(uint32_t slot, const void* data, uint32_t size_bytes);
    void Draw(uint32_t vertex_count, uint32_t instance_count = 1,
              uint32_t first_vertex = 0, uint32_t first_instance = 0);
    void DrawIndexed(uint32_t index_count, uint32_t instance_count = 1,
                      uint32_t first_index = 0, int32_t vertex_offset = 0,
                      uint32_t first_instance = 0);

    SDL_GPURenderPass*    SDLPass() const { return sdl_pass_; }
    SDL_GPUCommandBuffer* SDLCmd()  const { return sdl_cmd_; }

private:
    GpuPassView(SDL_GPURenderPass* rp, SDL_GPUCommandBuffer* cmd)
        : sdl_pass_(rp), sdl_cmd_(cmd) {}
    SDL_GPURenderPass*    sdl_pass_ = nullptr;
    SDL_GPUCommandBuffer* sdl_cmd_  = nullptr;
    friend class GpuRenderPass;
};
#endif // MD_SDL_GPU

// ─────────────────────────────────────────────────────────────────────────────
// GpuRenderPass — scoped render pass bound to one or more attachments.
// SDL_GPU: SDL_BeginGPURenderPass / SDL_EndGPURenderPass
// ─────────────────────────────────────────────────────────────────────────────
class GpuRenderPass {
public:
    struct DepthDesc {
        GpuDepthTexture* target;
        float clear_depth  = 1.0f;
        bool  cull_front   = false; // GL_FRONT for shadow bias (Peter-Panning fix)
        // Vulkan LOAD/STORE DONT_CARE pattern: set discard_after=true when depth is
        // not needed after this pass (e.g. shadow depth → resolved into EVSM moments).
        bool  discard_after = false;
        // true = LOAD instead of CLEAR (e.g. NPC Early-Z prepass reusing terrain
        // depth already copied into `target` by an earlier pass this frame).
        bool  load_depth    = false;
    };

    // Shadow / depth-only pass.
    // OpenGL: FBO bind + clear.
    void BeginDepthOnly(const DepthDesc& desc);

#ifdef MD_SDL_GPU
    // SDL_GPU depth-only variant: requires the frame command buffer.
    void BeginDepthOnly(SDL_GPUCommandBuffer* cmd, const DepthDesc& desc);
#endif

    // ── Color pass (main render target) ──────────────────────────────────────
    struct ColorDesc {
        float            clear[4]    = {0.f, 0.f, 0.f, 1.f};
        float            clear_depth = 1.0f;
        GpuDepthTexture* depth       = nullptr; // optional depth attachment
        bool             load_depth  = false;   // true = LOAD (e.g. after Early-Z prepass)
#ifdef MD_SDL_GPU
        // SDL_GPU: frame command buffer acquired from GpuDevice::AcquireCommandBuffer().
        SDL_GPUCommandBuffer* cmd = nullptr;
#endif
    };

    // SDL_GPU: AcquireSwapchainTexture + SDL_BeginGPURenderPass(color+depth).
    // OpenGL: bind FBO 0, save viewport, glClear color+depth.
    void BeginColor(const ColorDesc& desc);

    // SDL_GPU: SDL_EndGPURenderPass. OpenGL: restore FBO + viewport.
    void End();

#ifdef MD_SDL_GPU
    SDL_GPURenderPass* SDLPass() const { return sdl_pass_; }

    // Non-owning view for binding/drawing into this pass. `cmd` is the same
    // command buffer passed to Begin*() -- GpuRenderPass doesn't retain it
    // itself (only sdl_pass_), so the caller (who still holds it) supplies
    // it here. See GpuPassView's doc comment for why this exists.
    GpuPassView View(SDL_GPUCommandBuffer* cmd) const { return GpuPassView(sdl_pass_, cmd); }
#endif

private:
    int  saved_vp_[4]  = {};
    bool cull_front_   = false;
#ifdef MD_SDL_GPU
    SDL_GPURenderPass* sdl_pass_ = nullptr;
#endif
};


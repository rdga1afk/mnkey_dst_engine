#pragma once
// gpu_hal_free_functions.h — stateless SDL_GPU passthrough wrappers +
// SPIR-V/pipeline object caches (split from gpu_hal.h, code audit
// proposal #8). These take a command buffer or device directly (no HAL
// object to attach a method to) -- see each group's own comment below for
// why it isn't a GpuPassView/GpuCommandBuffer/GpuComputePass method.

#include <monkey_dust/render/gpu_device.h>
#include <cstdint>

#ifdef MD_SDL_GPU
#include <SDL3/SDL_gpu.h>
#endif

#ifdef MD_SDL_GPU
// ─────────────────────────────────────────────────────────────────────────────
// GpuPush{Vertex,Fragment,Compute}Uniforms — stateless passthrough.
//
// Unlike bind/draw calls, SDL_GPU's three push-uniform functions
// (SDL_PushGPUVertexUniformData / ...Fragment... / ...Compute...) take ONLY
// a command buffer, never a render pass or compute pass handle. So neither
// GpuPassView (pass-scoped) nor GpuComputePass (owns compute-pass lifecycle)
// is the right shape to wrap them -- forcing callers through either would
// require a pass object these calls don't need, including the 3 real M1
// call sites that push compute uniforms from functions with no GpuComputePass
// at all (raw SDL_BeginGPUComputePass, e.g. terrain_vt_page_cache.cpp). A
// bare free function matches what the SDL_GPU API itself expects.
inline void GpuPushVertexUniforms(SDL_GPUCommandBuffer* cmd, uint32_t slot,
                                   const void* data, uint32_t size_bytes) {
    if (cmd) SDL_PushGPUVertexUniformData(cmd, slot, data, size_bytes);
}
inline void GpuPushFragmentUniforms(SDL_GPUCommandBuffer* cmd, uint32_t slot,
                                     const void* data, uint32_t size_bytes) {
    if (cmd) SDL_PushGPUFragmentUniformData(cmd, slot, data, size_bytes);
}
inline void GpuPushComputeUniforms(SDL_GPUCommandBuffer* cmd, uint32_t slot,
                                    const void* data, uint32_t size_bytes) {
    if (cmd) SDL_PushGPUComputeUniformData(cmd, slot, data, size_bytes);
}

// GpuPushDebugGroup / GpuPopDebugGroup — 1:1, no semantics, same class as
// GpuPushVertexUniforms above (both SDL functions take only `cmd`). RenderDoc
// group labels for command-buffer regions with no debug info otherwise
// (SPIR-V builds carry none) -- currently unconditional in every build
// config, including release; a future candidate for gating behind
// MONKEY_DUST_EDITOR-style flag (submit-time cost, same logic as ImGui's own
// #ifdef gate) is NOT done here since that would be a behavior change, out
// of M1's scope.
inline void GpuPushDebugGroup(SDL_GPUCommandBuffer* cmd, const char* name) {
    if (cmd) SDL_PushGPUDebugGroup(cmd, name);
}
inline void GpuPopDebugGroup(SDL_GPUCommandBuffer* cmd) {
    if (cmd) SDL_PopGPUDebugGroup(cmd);
}

// ─────────────────────────────────────────────────────────────────────────────
// GpuMapTransfer / GpuUnmapTransfer — stateless passthrough, no RAII.
//
// `cycle` is a real per-call-site parameter, not a default: some callers need
// cycle=true (fresh backing memory per Map, avoids stalling on in-flight GPU
// reads), others need cycle=false (e.g. transform_soa.cpp's per-frame-slot
// staging buffers, where triple-buffering already guarantees the GPU never
// reads the slot being mapped -- forcing cycle=true there would be silently
// wrong, not just suboptimal). A RAII wrapper was considered and rejected:
// its destructor would call Unmap() at scope exit, not at the exact source
// line the raw call currently sits on, and a few of these call sites have
// real code between the memcpy and the existing Unmap() (error-path early
// returns, buffer-size branches) that scope-exit timing would silently
// change -- the same class of invisible-to-A/B, invisible-to-tests behavior
// shift documented for the malloc-failure branch in item 8 and for
// editor_screenshot.cpp's NotifyCommandBufferSubmitted() ordering.
inline void* GpuMapTransfer(SDL_GPUTransferBuffer* tb, bool cycle) {
    return SDL_MapGPUTransferBuffer(md::GpuDevice::Get().SDLDevice(), tb, cycle);
}
inline void GpuUnmapTransfer(SDL_GPUTransferBuffer* tb) {
    SDL_UnmapGPUTransferBuffer(md::GpuDevice::Get().SDLDevice(), tb);
}

// GpuCreateTransferBuffer / GpuReleaseTransferBuffer — stateless passthrough,
// no RAII, same reasoning as GpuMapTransfer/GpuUnmapTransfer above. `dev` is
// an explicit parameter here (unlike Map/Unmap, which fetch it from the
// GpuDevice singleton) because these call sites' own functions already take
// `dev` as a real parameter from their caller -- TerrainVtPageCache::Init,
// TerrainQuadtreeRenderer::UploadNodeData, TerrainWorldHeightmap's equivalent,
// etc. are exercised by isolated-device unit tests (see DEFECT_INVENTORY.md
// #1's cross-device fence bug for why "just use the singleton" is unsafe
// here) -- silently switching them to GpuDevice::Get().SDLDevice() would
// create transfer buffers on a different device than the one the caller
// explicitly chose.
inline SDL_GPUTransferBuffer* GpuCreateTransferBuffer(SDL_GPUDevice* dev,
                                                       const SDL_GPUTransferBufferCreateInfo* info) {
    return SDL_CreateGPUTransferBuffer(dev, info);
}
inline void GpuReleaseTransferBuffer(SDL_GPUDevice* dev, SDL_GPUTransferBuffer* tb) {
    SDL_ReleaseGPUTransferBuffer(dev, tb);
}

// GpuCreateShader / GpuReleaseShader / GpuCreateGraphicsPipeline /
// GpuReleaseGraphicsPipeline — 1:1, `dev` explicit, same reasoning as
// GpuCreateTransferBuffer above. Deliberately NOT routed through the
// GpuPipeline class (which owns the usual Desc-driven Create()/Destroy()
// used everywhere else in the tree): evsm_shadow.cpp's moment pipeline has
// its own raw SDL_GPUGraphicsPipelineCreateInfo assembly (custom
// position-only vertex layout, explicit per-shader uniform/sampler counts
// via a local LoadSpv() helper, partial-failure shader release on error)
// that predates GpuPipeline and doesn't map onto its Desc without
// re-deriving every field GpuPipeline::Create() sets internally --
// exactly the re-verification risk this session's classes avoid (same
// reasoning as item 9's texture/sampler classes not being retrofitted
// onto item 3's remainder). A 1:1 pair preserves the file's existing
// error-handling shape untouched.
inline SDL_GPUShader* GpuCreateShader(SDL_GPUDevice* dev, const SDL_GPUShaderCreateInfo* info) {
    return SDL_CreateGPUShader(dev, info);
}
inline void GpuReleaseShader(SDL_GPUDevice* dev, SDL_GPUShader* shader) {
    SDL_ReleaseGPUShader(dev, shader);
}
inline SDL_GPUGraphicsPipeline* GpuCreateGraphicsPipeline(SDL_GPUDevice* dev,
                                                           const SDL_GPUGraphicsPipelineCreateInfo* info) {
    return SDL_CreateGPUGraphicsPipeline(dev, info);
}
inline void GpuReleaseGraphicsPipeline(SDL_GPUDevice* dev, SDL_GPUGraphicsPipeline* pipeline) {
    SDL_ReleaseGPUGraphicsPipeline(dev, pipeline);
}

// GpuCreateTexture / GpuReleaseTexture / GpuCreateSampler / GpuReleaseSampler
// — 1:1, `dev` explicit, same reasoning as GpuCreateTransferBuffer above.
// Deliberately NOT routed through GpuColorTexture/GpuSampler (the owning
// classes item 9 built): those fit a "create once, this class's Shutdown()
// releases it" shape, but the real remainder here has genuinely different
// ownership per file -- terrain_renderer.cpp creates through a temporary
// wrapper object then calls .TakeSDLTexture()/.TakeSDLSampler() to move
// ownership into its own member fields (so ITS OWN Shutdown() controls
// release timing, not the temporary's destructor); several files release
// via a shared helper looping over multiple owned texture/sampler pairs.
// Forcing either pattern into GpuColorTexture/GpuSampler would mean adding
// a Take()-equivalent or a batch-release API neither class has today, for a
// one-off need -- the exact over-fitting this session's classes have
// avoided everywhere else (GpuPassView, GpuCopyPass, GpuComputePass::
// StorageBindings all grew from a real caller shape, never spec'd ahead of
// one). A 1:1 pair preserves whatever ownership shape each file already has.
inline SDL_GPUTexture* GpuCreateTexture(SDL_GPUDevice* dev, const SDL_GPUTextureCreateInfo* info) {
    return SDL_CreateGPUTexture(dev, info);
}
inline void GpuReleaseTexture(SDL_GPUDevice* dev, SDL_GPUTexture* tex) {
    SDL_ReleaseGPUTexture(dev, tex);
}
inline SDL_GPUSampler* GpuCreateSampler(SDL_GPUDevice* dev, const SDL_GPUSamplerCreateInfo* info) {
    return SDL_CreateGPUSampler(dev, info);
}
inline void GpuReleaseSampler(SDL_GPUDevice* dev, SDL_GPUSampler* sampler) {
    SDL_ReleaseGPUSampler(dev, sampler);
}

// GpuSetViewport / GpuSetScissor — 1:1, no semantics, same class as
// GpuPushDebugGroup above (both SDL functions take only the render pass).
// Free functions rather than GpuPassView/GpuCommandBuffer methods: every
// real call site already has a bare SDL_GPURenderPass* in scope (some via
// GpuPassView::FromRaw, some via GpuCommandBuffer::SDLPass(), one --
// world_map_ui.cpp's first scissor call -- before either wrapper exists yet
// in that function), so a method would force constructing/reordering a
// wrapper object purely to make this one call, for zero benefit over taking
// the pass pointer directly.
inline void GpuSetViewport(SDL_GPURenderPass* pass, const SDL_GPUViewport& vp) {
    if (pass) SDL_SetGPUViewport(pass, &vp);
}
inline void GpuSetScissor(SDL_GPURenderPass* pass, const SDL_Rect& rect) {
    if (pass) SDL_SetGPUScissor(pass, &rect);
}

// GpuWindowSupportsPresentMode / GpuSetSwapchainParameters — 1:1, `dev` +
// `win` explicit (window.h's window_set_vsync() already fetches both from
// GpuDevice::Get() right next to these calls, so this changes nothing about
// what the caller has to pass). window.h's own header comment ("Rule M-A:
// include only from Main.cpp and EditorMain.cpp") restricts who may INCLUDE
// window.h, not what window.h may depend on -- the file already calls
// GpuDevice::Get() directly in this same function, so it has zero isolation
// from gpu_hal concepts to begin with. Not a protected exception (confirmed
// during §3.1 planning), just the last swapchain/present-mode category that
// had never been categorized before this pass.
inline bool GpuWindowSupportsPresentMode(SDL_GPUDevice* dev, SDL_Window* win,
                                          SDL_GPUPresentMode mode) {
    return SDL_WindowSupportsGPUPresentMode(dev, win, mode);
}
inline bool GpuSetSwapchainParameters(SDL_GPUDevice* dev, SDL_Window* win,
                                       SDL_GPUSwapchainComposition composition,
                                       SDL_GPUPresentMode mode) {
    return SDL_SetGPUSwapchainParameters(dev, win, composition, mode);
}
#endif // MD_SDL_GPU

// ─────────────────────────────────────────────────────────────────────────────
// GpuDrawIndexedIndirect — indexed draw using a GPU-side indirect buffer.
// SDL_GPU: SDL_DrawGPUIndexedPrimitivesIndirect(pass, buf, offset, draw_count)
// ─────────────────────────────────────────────────────────────────────────────
void GpuDrawIndexedIndirect(unsigned int indirect_buf_id, uint32_t draw_count = 1);

// ── SPIR-V bytecode cache ─────────────────────────────────────────────────────
// GpuPipeline::Create() caches SPIR-V bytecode by path (FNV-1a hash) so that
// repeated pipeline creation (hot-reload, multiple passes sharing a shader)
// reads from memory instead of disk.  MAX 64 entries per session.
//
// MdSpvCache_Shutdown(): frees all cached bytecode buffers. Call at app exit.
// MdSpvCache_Stats(out): returns total cached count; fills *out if non-null.
void MdSpvCache_Shutdown();
// Evict one SPIR-V entry so the next Create/Reload re-reads it from disk.
void MdSpvCache_Invalidate(const char* glsl_path);
int  MdSpvCache_Stats(int* out_count = nullptr);

// Pipeline object cache (VkPipelineCache SDL_GPU adaptation).
// Caches SDL_GPUGraphicsPipeline objects by (vert+frag+features+raster) hash.
void MdPipeCache_Shutdown();
void MdPipeCache_Invalidate(const char* vert_path, const char* frag_path);


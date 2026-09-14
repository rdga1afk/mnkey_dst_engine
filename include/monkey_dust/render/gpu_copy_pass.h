#pragma once
// gpu_copy_pass.h — GpuCopyPass + mipmap/blit free functions (split from
// gpu_hal.h, code audit proposal #8). SDL_GPU only.

#include <cstdint>

#ifdef MD_SDL_GPU
#include <SDL3/SDL_gpu.h>

// ─────────────────────────────────────────────────────────────────────────────
// GpuCopyPass — scoped SDL_GPUCopyPass wrapper for upload/download/texture-copy.
// SDL_GPU only (OpenGL has no equivalent copy-pass concept).
//
// Real-shape survey before writing this (docs/HAL_CLOSURE_PROGRESS.md,
// "copy/upload/download/blit/mipmap group"): every real call site does
// exactly one of 5 operations inside an open SDL_GPUCopyPass — no repo
// call site does buffer-to-buffer copy, so that SDL_GPU function has no
// wrapper here (YAGNI, not an oversight). Struct params are passed through
// as the real SDL types (SDL_GPUTextureTransferInfo, SDL_GPUBufferRegion,
// etc.) verbatim — every call site already builds these inline, so wrapping
// them in a new type would be pure indirection, not abstraction.
//
// Two entry points, same reasoning as GpuPassView:
//   - Begin(cmd) — owns the pass, calls SDL_BeginGPUCopyPass/EndGPUCopyPass.
//     Use this for a single-purpose pass (most call sites: one upload, close).
//   - FromRaw(cp, cmd) — non-owning, for callers that receive an
//     ALREADY-OPEN SDL_GPUCopyPass* from someone else (confirmed real,
//     not hypothetical: GpuRingBuffer::UploadInPass, TerrainQuadtreeRenderer::
//     UploadNodeData, TerrainVtPageCache::UploadIndirectionRegion/
//     UploadPageMeta all take a caller-opened SDL_GPUCopyPass* today —
//     batching multiple uploads into one pass for perf). No End() call
//     needed/allowed on a FromRaw instance — the opener closes it.
// ─────────────────────────────────────────────────────────────────────────────
class GpuCopyPass {
public:
    static GpuCopyPass FromRaw(SDL_GPUCopyPass* cp, SDL_GPUCommandBuffer* cmd) {
        GpuCopyPass v;
        v.sdl_pass_ = cp;
        v.sdl_cmd_  = cmd;
        v.owns_pass_ = false;
        return v;
    }

    void Begin(SDL_GPUCommandBuffer* cmd) {
        sdl_cmd_   = cmd;
        sdl_pass_  = SDL_BeginGPUCopyPass(cmd);
        owns_pass_ = true;
    }
    void End() {
        if (owns_pass_ && sdl_pass_) SDL_EndGPUCopyPass(sdl_pass_);
        sdl_pass_ = nullptr;
    }

    void UploadTexture(const SDL_GPUTextureTransferInfo& src,
                        const SDL_GPUTextureRegion& dst, bool cycle = false) {
        if (sdl_pass_) SDL_UploadToGPUTexture(sdl_pass_, &src, &dst, cycle);
    }
    void UploadBuffer(const SDL_GPUTransferBufferLocation& src,
                       const SDL_GPUBufferRegion& dst, bool cycle = false) {
        if (sdl_pass_) SDL_UploadToGPUBuffer(sdl_pass_, &src, &dst, cycle);
    }
    void DownloadTexture(const SDL_GPUTextureRegion& src,
                          const SDL_GPUTextureTransferInfo& dst) {
        if (sdl_pass_) SDL_DownloadFromGPUTexture(sdl_pass_, &src, &dst);
    }
    void DownloadBuffer(const SDL_GPUBufferRegion& src,
                         const SDL_GPUTransferBufferLocation& dst) {
        if (sdl_pass_) SDL_DownloadFromGPUBuffer(sdl_pass_, &src, &dst);
    }
    void CopyTextureToTexture(const SDL_GPUTextureLocation& src,
                               const SDL_GPUTextureLocation& dst,
                               uint32_t w, uint32_t h, uint32_t d = 1,
                               bool cycle = false) {
        if (sdl_pass_) SDL_CopyGPUTextureToTexture(sdl_pass_, &src, &dst, w, h, d, cycle);
    }

    SDL_GPUCopyPass*      SDLPass() const { return sdl_pass_; }
    SDL_GPUCommandBuffer* SDLCmd()  const { return sdl_cmd_; }

private:
    SDL_GPUCopyPass*      sdl_pass_  = nullptr;
    SDL_GPUCommandBuffer* sdl_cmd_   = nullptr;
    bool                  owns_pass_ = false;
};

// GpuGenerateMipmaps / GpuBlitTexture — same shape as GpuPush*Uniforms above:
// both SDL functions take the command buffer directly, no pass object at
// all (confirmed by reading the SDL3 header, not assumed) — the second
// confirmed instance of this "operation binds to cmd, not to a pass" shape
// in this HAL (first was uniform-push). Forcing these through GpuCopyPass
// would require opening a pass neither function uses.
inline void GpuGenerateMipmaps(SDL_GPUCommandBuffer* cmd, SDL_GPUTexture* tex) {
    if (cmd && tex) SDL_GenerateMipmapsForGPUTexture(cmd, tex);
}
inline void GpuBlitTexture(SDL_GPUCommandBuffer* cmd, const SDL_GPUBlitInfo& info) {
    if (cmd) SDL_BlitGPUTexture(cmd, &info);
}
#endif // MD_SDL_GPU

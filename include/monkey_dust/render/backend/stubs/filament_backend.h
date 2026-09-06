#pragma once
#include <monkey_dust/render/backend/render_backend.h>
#include <monkey_dust/platform/md_log.h>

// RENDER-BACKEND-STAGE-4 (docs/RENDER_BACKEND_ABSTRACTION.md, §7). Empty
// scaffold only -- no real Filament embedding logic here (§9: separate
// future phase, not this one). Second priority of the 5 candidates:
// already influenced this codebase's architecture once before this phase
// (engine/include/monkey_dust/render/terrain_renderer.h's zone-layer LUT
// is a texture, not an SSBO, a 2026-08-09 decision made explicitly to
// narrow the Filament-incompatible compute/SSBO surface -- see that
// file's own doc comment, and docs/RENDER_BACKEND_ABSTRACTION.md §0.1).
// Compiles under MD_RENDER_BACKEND=FILAMENT but is never wired into
// game_init.cpp's runtime backend selection (that's a future phase).
namespace md::render_backend {

class FilamentBackend final : public IRenderBackend {
public:
    bool Init() override {
        MD_LOG(MD_LOG_WARNING, "[FilamentBackend] not implemented");
        return false;
    }
    void Shutdown() override {}
    const char* BackendName() const override { return "Filament (stub)"; }
    BackendCaps GetCaps() const override { return {}; }
    void SetFrameParams(const RenderFrameParams& /*params*/) override {}
    void UploadTransforms() override {
        MD_LOG(MD_LOG_WARNING, "[FilamentBackend] not implemented");
    }
    void RunGpuCulling() override {
        MD_LOG(MD_LOG_WARNING, "[FilamentBackend] not implemented");
    }
    void RunGpuSkinning() override {
        MD_LOG(MD_LOG_WARNING, "[FilamentBackend] not implemented");
    }
    void RenderShadowPass() override {
        MD_LOG(MD_LOG_WARNING, "[FilamentBackend] not implemented");
    }
    void RenderGBufferPass() override {
        MD_LOG(MD_LOG_WARNING, "[FilamentBackend] not implemented");
    }
    void RenderDeferredLighting() override {
        MD_LOG(MD_LOG_WARNING, "[FilamentBackend] not implemented");
    }
    void RunSsaoPass() override {
        MD_LOG(MD_LOG_WARNING, "[FilamentBackend] not implemented");
    }
    void RunPostProcessChain() override {
        MD_LOG(MD_LOG_WARNING, "[FilamentBackend] not implemented");
    }
    void RenderHud() override {
        MD_LOG(MD_LOG_WARNING, "[FilamentBackend] not implemented");
    }
    void RenderEditorOverlay() override {}
};

}  // namespace md::render_backend

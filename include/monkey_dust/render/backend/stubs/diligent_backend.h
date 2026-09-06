#pragma once
#include <monkey_dust/render/backend/render_backend.h>
#include <monkey_dust/platform/md_log.h>

// RENDER-BACKEND-STAGE-4 (docs/RENDER_BACKEND_ABSTRACTION.md, §7). Empty
// scaffold only -- no real Diligent Engine embedding logic here (§9:
// separate future phase, not this one). Same abstraction class as
// SdlGpuBackend/bgfx (explicit PSO, command buffer, multi-backend GPU
// API) -- IRenderBackend maps onto it directly (§7.1's own note). Compiles
// under MD_RENDER_BACKEND=DILIGENT but is never wired into game_init.cpp's
// runtime backend selection (that's a future phase).
namespace md::render_backend {

class DiligentBackend final : public IRenderBackend {
public:
    bool Init() override {
        MD_LOG(MD_LOG_WARNING, "[DiligentBackend] not implemented");
        return false;
    }
    void Shutdown() override {}
    const char* BackendName() const override { return "Diligent Engine (stub)"; }
    BackendCaps GetCaps() const override { return {}; }
    void SetFrameParams(const RenderFrameParams& /*params*/) override {}
    void UploadTransforms() override {
        MD_LOG(MD_LOG_WARNING, "[DiligentBackend] not implemented");
    }
    void RunGpuCulling() override {
        MD_LOG(MD_LOG_WARNING, "[DiligentBackend] not implemented");
    }
    void RunGpuSkinning() override {
        MD_LOG(MD_LOG_WARNING, "[DiligentBackend] not implemented");
    }
    void RenderShadowPass() override {
        MD_LOG(MD_LOG_WARNING, "[DiligentBackend] not implemented");
    }
    void RenderGBufferPass() override {
        MD_LOG(MD_LOG_WARNING, "[DiligentBackend] not implemented");
    }
    void RenderDeferredLighting() override {
        MD_LOG(MD_LOG_WARNING, "[DiligentBackend] not implemented");
    }
    void RunSsaoPass() override {
        MD_LOG(MD_LOG_WARNING, "[DiligentBackend] not implemented");
    }
    void RunPostProcessChain() override {
        MD_LOG(MD_LOG_WARNING, "[DiligentBackend] not implemented");
    }
    void RenderHud() override {
        MD_LOG(MD_LOG_WARNING, "[DiligentBackend] not implemented");
    }
    void RenderEditorOverlay() override {}
};

}  // namespace md::render_backend

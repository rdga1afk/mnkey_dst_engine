#pragma once
#include <monkey_dust/render/backend/render_backend.h>
#include <monkey_dust/platform/md_log.h>

// RENDER-BACKEND-STAGE-4 (docs/RENDER_BACKEND_ABSTRACTION.md, §7). Empty
// scaffold only -- no real bgfx embedding logic here (§9: separate future
// phase, not this one). Same abstraction class as SdlGpuBackend (explicit
// PSO, command buffer, multi-backend GPU API) -- IRenderBackend maps onto
// it most directly of the 5 candidates once real work starts (§7.1's own
// note); also already evaluated in docs/ENGINE_BENCHMARK.md as "ЗАПОЗИЧИТИ
// (ідея)" (see docs/RENDER_BACKEND_ABSTRACTION.md §0.1). Compiles under
// MD_RENDER_BACKEND=BGFX but is never wired into game_init.cpp's runtime
// backend selection (that's a future phase).
namespace md::render_backend {

class BgfxBackend final : public IRenderBackend {
public:
    bool Init() override {
        MD_LOG(MD_LOG_WARNING, "[BgfxBackend] not implemented");
        return false;
    }
    void Shutdown() override {}
    const char* BackendName() const override { return "bgfx (stub)"; }
    BackendCaps GetCaps() const override { return {}; }
    void SetFrameParams(const RenderFrameParams& /*params*/) override {}
    void UploadTransforms() override {
        MD_LOG(MD_LOG_WARNING, "[BgfxBackend] not implemented");
    }
    void RunGpuCulling() override {
        MD_LOG(MD_LOG_WARNING, "[BgfxBackend] not implemented");
    }
    void RunGpuSkinning() override {
        MD_LOG(MD_LOG_WARNING, "[BgfxBackend] not implemented");
    }
    void RenderShadowPass() override {
        MD_LOG(MD_LOG_WARNING, "[BgfxBackend] not implemented");
    }
    void RenderGBufferPass() override {
        MD_LOG(MD_LOG_WARNING, "[BgfxBackend] not implemented");
    }
    void RenderDeferredLighting() override {
        MD_LOG(MD_LOG_WARNING, "[BgfxBackend] not implemented");
    }
    void RunSsaoPass() override {
        MD_LOG(MD_LOG_WARNING, "[BgfxBackend] not implemented");
    }
    void RunPostProcessChain() override {
        MD_LOG(MD_LOG_WARNING, "[BgfxBackend] not implemented");
    }
    void RenderHud() override {
        MD_LOG(MD_LOG_WARNING, "[BgfxBackend] not implemented");
    }
    void RenderEditorOverlay() override {}
};

}  // namespace md::render_backend

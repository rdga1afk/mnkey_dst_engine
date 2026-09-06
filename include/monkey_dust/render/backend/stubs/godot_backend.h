#pragma once
#include <monkey_dust/render/backend/render_backend.h>
#include <monkey_dust/platform/md_log.h>

// RENDER-BACKEND-STAGE-4 (docs/RENDER_BACKEND_ABSTRACTION.md, §7). Empty
// scaffold only -- no real LibGodot embedding logic here (§9: separate
// future phase, not this one). Highest priority of the 5 candidates: the
// only one with a real done spike (LibGodot R1-R3, Intel HD 520, both
// renderers start without a crash, draw call cost ~3.7-3.9ms fits budget
// §13) -- see docs/RENDER_BACKEND_ABSTRACTION.md §0.1 for the full
// rationale. Compiles under MD_RENDER_BACKEND=GODOT but is never wired
// into game_init.cpp's runtime backend selection (that's a future phase).
namespace md::render_backend {

class GodotBackend final : public IRenderBackend {
public:
    bool Init() override {
        MD_LOG(MD_LOG_WARNING, "[GodotBackend] not implemented");
        return false;
    }
    void Shutdown() override {}
    const char* BackendName() const override { return "LibGodot (stub)"; }
    BackendCaps GetCaps() const override { return {}; }
    void SetFrameParams(const RenderFrameParams& /*params*/) override {}
    void UploadTransforms() override {
        MD_LOG(MD_LOG_WARNING, "[GodotBackend] not implemented");
    }
    void RunGpuCulling() override {
        MD_LOG(MD_LOG_WARNING, "[GodotBackend] not implemented");
    }
    void RunGpuSkinning() override {
        MD_LOG(MD_LOG_WARNING, "[GodotBackend] not implemented");
    }
    void RenderShadowPass() override {
        MD_LOG(MD_LOG_WARNING, "[GodotBackend] not implemented");
    }
    void RenderGBufferPass() override {
        MD_LOG(MD_LOG_WARNING, "[GodotBackend] not implemented");
    }
    void RenderDeferredLighting() override {
        MD_LOG(MD_LOG_WARNING, "[GodotBackend] not implemented");
    }
    void RunSsaoPass() override {
        MD_LOG(MD_LOG_WARNING, "[GodotBackend] not implemented");
    }
    void RunPostProcessChain() override {
        MD_LOG(MD_LOG_WARNING, "[GodotBackend] not implemented");
    }
    void RenderHud() override {
        MD_LOG(MD_LOG_WARNING, "[GodotBackend] not implemented");
    }
    void RenderEditorOverlay() override {}
};

}  // namespace md::render_backend

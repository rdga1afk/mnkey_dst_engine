#pragma once
#include <monkey_dust/render/backend/render_backend.h>
#include <monkey_dust/platform/md_log.h>

// RENDER-BACKEND-STAGE-4 (docs/RENDER_BACKEND_ABSTRACTION.md, §7). Empty
// scaffold only -- no real OGRE-Next embedding logic here (§9: separate
// future phase, not this one). Requires a material-conversion layer
// (MaterialDesc -> Hlms, §9) before real work can start, unlike bgfx/
// Diligent's more direct mapping -- but has full production-level
// features (forward+/clustered), unlike the immediate-mode libraries
// (§7.1's own note). Real Kenshi used OGRE3D + the adVantage Chunked-LOD
// plugin specifically for terrain (docs/RE_KENSHI_ADVANTAGE_TERRAIN
// reference), a thematic tie worth noting for planning even though it's
// not a factor in this phase's stub. Compiles under
// MD_RENDER_BACKEND=OGRENEXT but is never wired into game_init.cpp's
// runtime backend selection (that's a future phase).
namespace md::render_backend {

class OgreNextBackend final : public IRenderBackend {
public:
    bool Init() override {
        MD_LOG(MD_LOG_WARNING, "[OgreNextBackend] not implemented");
        return false;
    }
    void Shutdown() override {}
    const char* BackendName() const override { return "OGRE-Next (stub)"; }
    BackendCaps GetCaps() const override { return {}; }
    void SetFrameParams(const RenderFrameParams& /*params*/) override {}
    void UploadTransforms() override {
        MD_LOG(MD_LOG_WARNING, "[OgreNextBackend] not implemented");
    }
    void RunGpuCulling() override {
        MD_LOG(MD_LOG_WARNING, "[OgreNextBackend] not implemented");
    }
    void RunGpuSkinning() override {
        MD_LOG(MD_LOG_WARNING, "[OgreNextBackend] not implemented");
    }
    void RenderShadowPass() override {
        MD_LOG(MD_LOG_WARNING, "[OgreNextBackend] not implemented");
    }
    void RenderGBufferPass() override {
        MD_LOG(MD_LOG_WARNING, "[OgreNextBackend] not implemented");
    }
    void RenderDeferredLighting() override {
        MD_LOG(MD_LOG_WARNING, "[OgreNextBackend] not implemented");
    }
    void RunSsaoPass() override {
        MD_LOG(MD_LOG_WARNING, "[OgreNextBackend] not implemented");
    }
    void RunPostProcessChain() override {
        MD_LOG(MD_LOG_WARNING, "[OgreNextBackend] not implemented");
    }
    void RenderHud() override {
        MD_LOG(MD_LOG_WARNING, "[OgreNextBackend] not implemented");
    }
    void RenderEditorOverlay() override {}
};

}  // namespace md::render_backend

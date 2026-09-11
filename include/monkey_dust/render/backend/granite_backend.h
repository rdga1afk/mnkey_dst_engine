#pragma once
#include "render_backend.h"

// RENDER-BACKEND-STAGE-5 (docs/GRANITE_IRENDERBACKEND_INTEGRATION.md §2.2).
// GraniteBackend -- wraps the existing Granite Vulkan device/WSI dual-run
// code (monkey_dust/render/granite_backend.h -- note: different path, the
// pre-existing md::GraniteBackend singleton, NOT this class) into the
// IRenderBackend contract, same shape as SdlGpuBackend.
//
// Init()/Shutdown() delegate to md::GraniteBackend::Get() -- this class
// does not own or (re)create the Vulkan device/WSI itself (same "adapter,
// not rewrite" reasoning as SdlGpuBackend's own Init() doc comment): the
// caller is responsible for calling md::GraniteBackend::Get().Init(window)
// BEFORE constructing/using this class (IRenderBackend::Init() is zero-arg
// by contract, and engine/ cannot include platform/window.h itself -- §M-A
// isolation rule, root CLAUDE.md).
//
// Content draw calls: per docs/GRANITE_IRENDERBACKEND_INTEGRATION.md §2.2,
// Granite is confirmed NOT gated (explicit draw calls, GRANITE_BACKEND_
// COMPARISON.md §6.4) -- the SAME Set<Stage>PassCallback(BackendStageFn,
// void*) mechanism SdlGpuBackend uses applies directly, no scene-sync layer
// needed. This phase (Крок 2) only adds the class scaffold and callback
// dispatch plumbing -- no game/-side callback is registered against this
// backend yet (that wiring, and MD_RENDER_BACKEND=GRANITE CMake selection,
// is Крок 4, a separate future phase -- §4 "ЩО НЕ РОБИТИ").
namespace md::render_backend {

class GraniteBackend final : public IRenderBackend {
public:
    bool Init() override;
    void Shutdown() override;
    const char* BackendName() const override { return "Granite"; }
    BackendCaps GetCaps() const override;

    void SetFrameParams(const RenderFrameParams& params) override;

    // Same 5 callback slots as SdlGpuBackend (sdl_gpu_backend.h) -- same
    // signature, same "game/-side registers its own real logic once"
    // pattern. See SdlGpuBackend's doc comments for the full reasoning
    // behind why UploadAndSkin/DrawDeferredPasses fold into one callback
    // each rather than splitting 1:1 with IRenderBackend's method names.
    void SetShadowPassCallback(BackendStageFn fn, void* user) {
        shadow_pass_fn_ = fn;
        shadow_pass_user_ = user;
    }
    void SetGBufferPassCallback(BackendStageFn fn, void* user) {
        gbuffer_pass_fn_ = fn;
        gbuffer_pass_user_ = user;
    }
    void SetDeferredPassCallback(BackendStageFn fn, void* user) {
        deferred_pass_fn_ = fn;
        deferred_pass_user_ = user;
    }
    void SetUploadSkinCallback(BackendStageFn fn, void* user) {
        upload_skin_fn_ = fn;
        upload_skin_user_ = user;
    }
    void SetCullCallback(BackendStageFn fn, void* user) {
        cull_fn_ = fn;
        cull_user_ = user;
    }

    void UploadTransforms() override;
    void RunGpuCulling() override;
    void RunGpuSkinning() override;
    void RenderShadowPass() override;
    void RenderGBufferPass() override;
    void RenderDeferredLighting() override;
    void RunSsaoPass() override;
    void RunPostProcessChain() override;
    void RenderHud() override;
    void RenderEditorOverlay() override;

private:
    RenderFrameParams frame_params_;

    BackendStageFn shadow_pass_fn_   = nullptr;
    void*          shadow_pass_user_ = nullptr;
    BackendStageFn gbuffer_pass_fn_   = nullptr;
    void*          gbuffer_pass_user_ = nullptr;
    BackendStageFn deferred_pass_fn_   = nullptr;
    void*          deferred_pass_user_ = nullptr;
    BackendStageFn upload_skin_fn_   = nullptr;
    void*          upload_skin_user_ = nullptr;
    BackendStageFn cull_fn_          = nullptr;
    void*          cull_user_        = nullptr;
};

}  // namespace md::render_backend

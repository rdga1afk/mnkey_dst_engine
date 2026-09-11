#include <monkey_dust/render/backend/granite_backend.h>
#include <monkey_dust/render/granite_backend.h>
#include <monkey_dust/platform/md_log.h>

// RENDER-BACKEND-STAGE-5 (docs/GRANITE_IRENDERBACKEND_INTEGRATION.md §2.2).
// See the header's doc comment for the Init()/Shutdown() delegation
// reasoning. Stage-method bodies mirror SdlGpuBackend::*'s callback-
// dispatch pattern exactly (sdl_gpu_backend.cpp) -- copied, not
// reinvented, per the same "adapter, not rewrite" principle.
namespace md::render_backend {

bool GraniteBackend::Init() {
    // Does not call md::GraniteBackend::Get().Init(window) itself -- that
    // needs an SDL_Window*, which IRenderBackend::Init() (zero-arg by
    // contract) cannot receive, and engine/ isn't allowed to fetch it via
    // platform/window.h directly (§M-A isolation). The caller must already
    // have initialized md::GraniteBackend::Get() before using this class.
    if (!md::GraniteBackend::Get().IsBuilt()) {
        MD_LOG(MD_LOG_WARNING, "[GraniteBackend] Init(): not built (USE_GRANITE=OFF)");
        return false;
    }
    if (!md::GraniteBackend::Get().IsReady()) {
        MD_LOG(MD_LOG_WARNING, "[GraniteBackend] Init(): md::GraniteBackend not yet "
               "initialized by caller (expected md::GraniteBackend::Get().Init(window) first)");
        return false;
    }
    return true;
}

void GraniteBackend::Shutdown() {
    // Does not call md::GraniteBackend::Get().Shutdown() itself -- same
    // non-ownership reasoning as Init(): the caller that initialized the
    // window-bound Granite device owns its teardown too (matches
    // SdlGpuBackend::Shutdown() never touching GpuDevice::Get().Shutdown()
    // either).
}

BackendCaps GraniteBackend::GetCaps() const {
    // {} matches SdlGpuBackend::GetCaps()'s own current state -- no
    // Set<Stage>PassCallback is registered against this backend yet (Крок 4,
    // out of scope here), so reporting any true capability would be
    // premature (docs/GRANITE_IRENDERBACKEND_INTEGRATION.md §4).
    return {};
}

void GraniteBackend::SetFrameParams(const RenderFrameParams& params) {
    frame_params_ = params;
}

void GraniteBackend::UploadTransforms() {
    if (upload_skin_fn_) {
        upload_skin_fn_(upload_skin_user_, frame_params_);
    } else {
        MD_LOG(MD_LOG_WARNING, "[GraniteBackend] UploadTransforms: no callback registered");
    }
}

void GraniteBackend::RunGpuCulling() {
    if (cull_fn_) {
        cull_fn_(cull_user_, frame_params_);
    } else {
        MD_LOG(MD_LOG_WARNING, "[GraniteBackend] RunGpuCulling: no callback registered");
    }
}

void GraniteBackend::RunGpuSkinning() {
    // Deliberate no-op -- folded into UploadTransforms(), same UploadAndSkin
    // single-function reasoning as SdlGpuBackend::RunGpuSkinning().
}

void GraniteBackend::RenderShadowPass() {
    if (shadow_pass_fn_) {
        shadow_pass_fn_(shadow_pass_user_, frame_params_);
    } else {
        MD_LOG(MD_LOG_WARNING, "[GraniteBackend] RenderShadowPass: no callback registered");
    }
}

void GraniteBackend::RenderGBufferPass() {
    if (gbuffer_pass_fn_) {
        gbuffer_pass_fn_(gbuffer_pass_user_, frame_params_);
    } else {
        MD_LOG(MD_LOG_WARNING, "[GraniteBackend] RenderGBufferPass: no callback registered");
    }
}

void GraniteBackend::RenderDeferredLighting() {
    if (deferred_pass_fn_) {
        deferred_pass_fn_(deferred_pass_user_, frame_params_);
    } else {
        MD_LOG(MD_LOG_WARNING, "[GraniteBackend] RenderDeferredLighting: no callback registered");
    }
}

void GraniteBackend::RunSsaoPass() {
    // Deliberate no-op -- folded into RenderDeferredLighting(), same
    // DrawDeferredPasses single-function reasoning as SdlGpuBackend.
}

void GraniteBackend::RunPostProcessChain() {
    // Deliberate no-op -- folded into RenderDeferredLighting(), same
    // DrawDeferredPasses single-function reasoning as SdlGpuBackend.
}

void GraniteBackend::RenderHud() {
    MD_LOG(MD_LOG_WARNING, "[GraniteBackend] RenderHud: not implemented");
}

void GraniteBackend::RenderEditorOverlay() {
    // RENDER-BACKEND-STAGE-6 (docs/GRANITE_IRENDERBACKEND_INTEGRATION.md
    // §2.3): the registered callback (tools/editor/editor_granite_imgui_
    // bridge.cpp) does its own ImGui::NewFrame()/content/ImGui::Render()
    // and then calls md::GraniteBackend::Get().RenderFrameWithOverlay() --
    // that call OWNS Granite's entire per-frame sequence itself (begin_frame
    // through end_frame), unlike every other stage method here which draws
    // into an already-active frame_params_.cmd. frame_params_ is passed
    // through regardless so the callback can still read camera/viewport/etc
    // if it ever needs to (not used by the current ImGui bridge).
    if (editor_overlay_fn_) {
        editor_overlay_fn_(editor_overlay_user_, frame_params_);
    } else {
        MD_LOG(MD_LOG_WARNING, "[GraniteBackend] RenderEditorOverlay: no callback registered");
    }
}

}  // namespace md::render_backend

#include <monkey_dust/render/backend/sdl_gpu_backend.h>
#include <monkey_dust/platform/md_log.h>

// RENDER-BACKEND-STAGE-1 (docs/RENDER_BACKEND_ABSTRACTION.md, §4).
// Порожня оболонка -- усі методи логують і повертають без реальної
// логіки. Ще НЕ підключено до game_render_frame.cpp (жоден з цих методів
// не викликається з game/ на цьому етапі) -- реальні call site
// переносяться сюди по одному в Етапі 2 (§5).
namespace md::render_backend {

bool SdlGpuBackend::Init() {
    MD_LOG(MD_LOG_WARNING, "[SdlGpuBackend] stub: %s", __func__);
    return false;
}

void SdlGpuBackend::Shutdown() {
    MD_LOG(MD_LOG_WARNING, "[SdlGpuBackend] stub: %s", __func__);
}

BackendCaps SdlGpuBackend::GetCaps() const {
    MD_LOG(MD_LOG_WARNING, "[SdlGpuBackend] stub: %s", __func__);
    return {};
}

void SdlGpuBackend::SetFrameParams(const RenderFrameParams& params) {
    MD_LOG(MD_LOG_WARNING, "[SdlGpuBackend] stub: %s", __func__);
    frame_params_ = params;
}

void SdlGpuBackend::UploadTransforms() {
    MD_LOG(MD_LOG_WARNING, "[SdlGpuBackend] stub: %s", __func__);
}

void SdlGpuBackend::RunGpuCulling() {
    MD_LOG(MD_LOG_WARNING, "[SdlGpuBackend] stub: %s", __func__);
}

void SdlGpuBackend::RunGpuSkinning() {
    MD_LOG(MD_LOG_WARNING, "[SdlGpuBackend] stub: %s", __func__);
}

void SdlGpuBackend::RenderShadowPass() {
    // RENDER-BACKEND-STAGE-2a: реальна логіка (ShadowSystem::Update +
    // NpcRender::DrawShadowMaps) живе в game/, зареєстрована через
    // SetShadowPassCallback() -- див. render_backend_types.h's
    // BackendStageFn doc-коментар для повного обґрунтування.
    if (shadow_pass_fn_) {
        shadow_pass_fn_(shadow_pass_user_, frame_params_);
    } else {
        MD_LOG(MD_LOG_WARNING, "[SdlGpuBackend] RenderShadowPass: no callback registered");
    }
}

void SdlGpuBackend::RenderGBufferPass() {
    // RENDER-BACKEND-STAGE-2b/2f (docs/RENDER_BACKEND_ABSTRACTION.md §5):
    // NpcRender::DrawScene, same callback-based IoC as RenderShadowPass --
    // see render_backend_types.h's RenderFrameParams doc comment for why
    // this stage needed more fields (player_entity/selected/cam_az/
    // cam_x_io/cam_z_io/frame_ctx) than the shadow pass did.
    if (gbuffer_pass_fn_) {
        gbuffer_pass_fn_(gbuffer_pass_user_, frame_params_);
    } else {
        MD_LOG(MD_LOG_WARNING, "[SdlGpuBackend] RenderGBufferPass: no callback registered");
    }
}

void SdlGpuBackend::RenderDeferredLighting() {
    // RENDER-BACKEND-STAGE-2c/2d: covers ambient + motion-prep + SSAO +
    // bloom + motion-blur-apply ALL AT ONCE -- see SetDeferredPassCallback's
    // doc comment (sdl_gpu_backend.h) for why this doesn't split across
    // RenderDeferredLighting()/RunSsaoPass()/RunPostProcessChain() the way
    // their names suggest: DrawDeferredPasses is one function with shared
    // state (bloom_active/mb_active, AcquireSwapchainCached's "once per
    // command buffer" rule) that a 3-way split would either duplicate or
    // multiply-execute.
    if (deferred_pass_fn_) {
        deferred_pass_fn_(deferred_pass_user_, frame_params_);
    } else {
        MD_LOG(MD_LOG_WARNING, "[SdlGpuBackend] RenderDeferredLighting: no callback registered");
    }
}

void SdlGpuBackend::RunSsaoPass() {
    // Deliberate no-op for this backend -- folded into RenderDeferredLighting()
    // (same DrawDeferredPasses call), see that method's doc comment.
}

void SdlGpuBackend::RunPostProcessChain() {
    // Deliberate no-op for this backend -- folded into RenderDeferredLighting()
    // (same DrawDeferredPasses call), see that method's doc comment.
}

void SdlGpuBackend::RenderHud() {
    MD_LOG(MD_LOG_WARNING, "[SdlGpuBackend] stub: %s", __func__);
}

void SdlGpuBackend::RenderEditorOverlay() {
    MD_LOG(MD_LOG_WARNING, "[SdlGpuBackend] stub: %s", __func__);
}

}  // namespace md::render_backend

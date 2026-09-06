#pragma once
#include "render_backend.h"

// RENDER-BACKEND-STAGE-0/1 (docs/RENDER_BACKEND_ABSTRACTION.md, §3.2/§4).
// SdlGpuBackend -- єдина РЕАЛЬНА реалізація IRenderBackend цієї фази.
// Delegates у ІСНУЮЧІ підсистеми (GBuffer, DeferredLightingSystem,
// ShadowSystem, NpcGpuCuller/CullAndPrepass, TerrainRenderer, PropRenderer,
// ParticleRenderer, SSAOSystem, MotionBlurSystem, BloomSystem, EvsmShadow)
// -- ЖОДНА з цих систем не переписується, тільки викликається з нового
// місця (§2.1, "adapter, не rewrite"). Етап 1: усі методи -- порожні
// stub'и (MD_LOG_WARNING + return), ще НЕ підключено до
// game_render_frame.cpp. Реальні call site переносяться туди по одному,
// під-стадія за під-стадією, в Етапі 2 (§5).
namespace md::render_backend {

class SdlGpuBackend final : public IRenderBackend {
public:
    bool Init() override;
    void Shutdown() override;
    const char* BackendName() const override { return "SDL_GPU"; }
    BackendCaps GetCaps() const override;

    void SetFrameParams(const RenderFrameParams& params) override;

    // Callback-based інверсія контролю (Етап 2a, §5's архітектурне
    // рішення): реальна логіка живе в game/ (NpcRender::DrawShadowMaps
    // звертається до приватних game/-членів), engine/ не може викликати
    // її напряму. game/-сторона реєструє СВОЮ функцію ОДИН раз (типово
    // при NpcRender::Init() чи еквіваленті); RenderShadowPass() нижче
    // просто викликає зареєстрований callback з поточним frame_params_.
    void SetShadowPassCallback(BackendStageFn fn, void* user) {
        shadow_pass_fn_ = fn;
        shadow_pass_user_ = user;
    }

    // Etap 2b: NpcRender::DrawScene (also draws terrain/props/particles --
    // §3.2's own doc comment on RenderGBufferPass explains this is one call
    // site covering both 2b and 2f, not two independent ones).
    void SetGBufferPassCallback(BackendStageFn fn, void* user) {
        gbuffer_pass_fn_ = fn;
        gbuffer_pass_user_ = user;
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
    // internal state -- вказівники/handle на існуючі системи, БЕЗ
    // дублювання даних (GBuffer&, ShadowSystem&, тощо через посилання/
    // singleton доступ). Заповнюється Етапом 2, коли реальні call site
    // переносяться сюди.
    RenderFrameParams frame_params_;

    // Callback-слоти (Етап 2, по одному на під-стадію -- додаються тут
    // ЛИШЕ коли конкретна під-стадія реально переноситься, не всі 10
    // наперед). 2a:
    BackendStageFn shadow_pass_fn_   = nullptr;
    void*          shadow_pass_user_ = nullptr;
    // 2b:
    BackendStageFn gbuffer_pass_fn_   = nullptr;
    void*          gbuffer_pass_user_ = nullptr;
};

}  // namespace md::render_backend

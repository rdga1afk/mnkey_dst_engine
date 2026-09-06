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
    // переносяться сюди -- Етап 0/1 не потребує жодного поля тут ще.
    RenderFrameParams frame_params_;
};

}  // namespace md::render_backend

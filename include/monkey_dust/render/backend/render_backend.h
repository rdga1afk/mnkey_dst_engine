#pragma once
#include "render_backend_types.h"

// RENDER-BACKEND-STAGE-0 (docs/RENDER_BACKEND_ABSTRACTION.md, §3.2).
// IRenderBackend -- один інтерфейс виклику GPU render pipeline, щоб
// SdlGpuBackend (сьогодні) і майбутні GodotBackend/FilamentBackend/
// OgreNextBackend/BgfxBackend/O3deAtomBackend stub-класи (§7) підключались
// через ту саму точку виклику з game_render_frame.cpp, не переписуючи
// ігрову логіку/ECS/asset pipeline.
//
// Метод-на-етап-пайплайну (НЕ один monolithic RenderFrame()) -- порядок
// методів нижче ВІДПОВІДАЄ реальному порядку викликів у грі, перевірено
// проти коду 2026-09-06 (§3.1's "Реальна послідовність"), не вигадано:
//   game_render_frame.cpp -> NpcRender::RenderFrame()
//   (game/src/render/npc_render_deferred.cpp:188) -> UploadAndSkin/
//   CullAndPrepass/DrawScene/DrawDeferredPasses.
//
// ДВА реальні call site НЕ розділяються так чисто, як окремі методи тут
// натякають -- задокументовано явно, а не приховано:
//   - UploadTransforms()/RunGpuSkinning() -- ОБИДВА це UploadAndSkin()
//     (npc_render_deferred.cpp:264), одна функція. Етап 2e доведеться АБО
//     викликати UploadAndSkin() з обох методів (дублює роботу), АБО
//     розділити саму функцію на дві половини під час переносу.
//   - RenderGBufferPass() -- це DrawScene() (npc_render_draw_scene.cpp),
//     яка ТАКОЖ малює terrain/props/particles. Етапи 2b і 2f (§5.1) -- це
//     один і той самий call site, не два незалежні.
namespace md::render_backend {

class IRenderBackend {
public:
    virtual ~IRenderBackend() = default;

    // Ініціалізація/завершення.
    virtual bool Init() = 0;
    virtual void Shutdown() = 0;
    virtual const char* BackendName() const = 0;
    virtual BackendCaps GetCaps() const = 0;

    // Встановлює параметри поточного кадру -- викликається ОДИН раз перед
    // послідовністю методів нижче (кожен з них zero-arg за дизайном §3.2).
    virtual void SetFrameParams(const RenderFrameParams& params) = 0;

    // Крок 7e (частина): TransformSoA upload -- реально це половина
    // UploadAndSkin() (npc_render_deferred.cpp:264), не окремий call site.
    virtual void UploadTransforms() = 0;
    // Крок 7h: CullAndPrepass() -- GPU compute cull.
    virtual void RunGpuCulling() = 0;
    // Крок 7e (частина): GPU-скінінг -- інша половина UploadAndSkin().
    virtual void RunGpuSkinning() = 0;
    // Крок 7g: ShadowSystem::Update() + DrawShadowMaps().
    virtual void RenderShadowPass() = 0;
    // Крок 7i: DrawScene() -- ТАКОЖ terrain/props/particles, не лише GBuffer.
    virtual void RenderGBufferPass() = 0;
    // Крок 7j (частина): DrawAmbientPass() усередині DrawDeferredPasses().
    virtual void RenderDeferredLighting() = 0;
    // Крок 7j (частина): SSAO Prep0/Prep1/Main/Blur/Apply -- 5 проходів,
    // один виклик методу тут.
    virtual void RunSsaoPass() = 0;
    // Крок 7j (частина): Motion Prep + Bloom (Compute/Composite) + Motion
    // Blur Apply. НЕ CAS, НЕ SMAA -- жодне з двох не існує в
    // game/src/render/*.cpp (перевірено grep 2026-09-06, нуль збігів;
    // CasPass реально існує, але лише в tools/flare_demo, поза скоупом
    // цієї фази -- §9).
    virtual void RunPostProcessChain() = 0;
    // МОЖЕ не відповідати живому call site у non-editor release-білді --
    // MdDraw2D-based HUD весь під #ifndef MD_SDL_GPU (мертвий код,
    // MD_SDL_GPU єдина буксована конфігурація). Реальний HUD зараз --
    // ImGui minimap/dialog всередині game_render_frame.cpp's кроку 3
    // (EditorImGui), не окремий GPU-виклик.
    virtual void RenderHud() = 0;
    // no-op якщо !MONKEY_DUST_EDITOR.
    virtual void RenderEditorOverlay() = 0;
};

}  // namespace md::render_backend

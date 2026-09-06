#pragma once
#include <monkey_dust/render/md_camera.h>
#include <monkey_dust/render/gpu_hal.h>

// RENDER-BACKEND-STAGE-0 (docs/RENDER_BACKEND_ABSTRACTION.md, §3.2).
// POD-типи, спільні для всіх backend-реалізацій (SdlGpuBackend зараз,
// GodotBackend/FilamentBackend/OgreNextBackend/BgfxBackend/O3deAtomBackend
// stub-класи пізніше). БЕЗ std::vector/std::string у hot-path структурах.
namespace md::render_backend {

// Per-frame parameters IRenderBackend's granular methods need but don't
// take as their own arguments (кожен метод §3.2 — zero-arg, per документ).
// Викликач (реально: NpcRender::RenderFrame(), game/src/render/
// npc_render_deferred.cpp:188 — не game_render_frame.cpp напряму,
// §3.1's виправлена послідовність) встановлює це ОДИН раз на кадр перед
// послідовністю UploadTransforms()/RunGpuCulling()/...
//
// cmd/active_count -- додано під час Етапу 2a (2026-09-06), НЕ Етапу 0:
// перший реальний перенесений call site (RenderShadowPass) виявив, що
// без активного command buffer жоден метод не може реально щось
// намалювати. НЕ NpcRender::FrameCtx напряму (game/'s тип) -- engine/
// не залежить від game/ (split-readiness, CLAUDE.md) -- лише
// GpuCommandBufferHandle, вже engine-типу (gpu_hal.h). Викликач
// конвертує свій FrameCtx.cmd/ac у ці поля перед SetFrameParams().
//
// camera -- невласницький вказівник, час життя керується викликачем
// (той самий MdCamera& active_cam/cam, що вже живе в NpcRender::
// RenderFrame() протягом усього кадру).
struct RenderFrameParams {
    const MdCamera*            camera       = nullptr;
    float                      cam_x        = 0.f;
    float                      cam_z        = 0.f;
    float                      now_s        = 0.f;
    float                      dt           = 0.f;
    int                        viewport_w   = 0;
    int                        viewport_h   = 0;
    md::GpuCommandBufferHandle cmd          = nullptr;
    int                        active_count = 0;  // FrameCtx::ac (active NPC count)
};

// Backend capability flags -- дозволяє викликачу (майбутній Етап 3+)
// пропускати виклики методів, яких конкретний backend не підтримує,
// замість покладатись на кожен stub мовчки no-op'ити.
struct BackendCaps {
    bool supports_deferred      = false;
    bool supports_gpu_culling   = false;
    bool supports_shadows       = false;
    bool supports_post_process  = false;
};

// Callback-based інверсія контролю для SdlGpuBackend (Етап 2, архітектурне
// рішення 2026-09-06, owner-вибір "Варіант 2"): реальний рендер-код
// (NpcRender::DrawShadowMaps і т.п.) звертається до ПРИВАТНИХ членів
// game/-класів -- SdlGpuBackend (engine/) фізично не може викликати їх
// напряму без порушення "engine/ не залежить від game/". game/-сторона
// реєструє СВОЮ реальну логіку як звичайний C-функційний покажчик (НЕ
// std::function -- може приховано алокувати в купі, якщо замикання
// перевищує SBO-буфер, заборонено в hot-path per CLAUDE.md) + опційний
// user-контекст. Той самий патерн, що вже є в проєкті:
// TerrainQuadtree::HeightSampleFn (engine/include/monkey_dust/world/
// terrain_quadtree.h). Один спільний сигнатурний тип для ВСІХ стадій --
// кожен callback читає все потрібне з уже наявного RenderFrameParams,
// не потребує власного custom-типу параметрів.
using BackendStageFn = void (*)(void* user, const RenderFrameParams& params);

}  // namespace md::render_backend

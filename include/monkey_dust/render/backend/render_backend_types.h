#pragma once
#include <monkey_dust/render/md_camera.h>

// RENDER-BACKEND-STAGE-0 (docs/RENDER_BACKEND_ABSTRACTION.md, §3.2).
// POD-типи, спільні для всіх backend-реалізацій (SdlGpuBackend зараз,
// GodotBackend/FilamentBackend/OgreNextBackend/BgfxBackend/O3deAtomBackend
// stub-класи пізніше). БЕЗ std::vector/std::string у hot-path структурах.
namespace md::render_backend {

// Per-frame parameters IRenderBackend's granular methods need but don't
// take as their own arguments (кожен метод §3.2 — zero-arg, per документ).
// Викликач (game_render_frame.cpp, після Етапу 2/3) встановлює це ОДИН
// раз на кадр перед послідовністю UploadTransforms()/RunGpuCulling()/...
// -- поля відповідають РЕАЛЬНИМ параметрам NpcRender::RenderFrame()
// (game/src/render/npc_render_deferred.cpp:188), не вигаданому набору.
// camera -- невласницький вказівник, час життя керується викликачем
// (той самий MdCamera& active_cam, що вже живе в game_render_frame.cpp
// протягом усього кадру).
struct RenderFrameParams {
    const MdCamera* camera    = nullptr;
    float           cam_x     = 0.f;
    float           cam_z     = 0.f;
    float           now_s     = 0.f;
    float           dt        = 0.f;
    int             viewport_w = 0;
    int             viewport_h = 0;
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

}  // namespace md::render_backend

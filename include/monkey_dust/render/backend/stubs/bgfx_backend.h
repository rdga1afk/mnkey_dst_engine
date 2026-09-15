#pragma once
#include <monkey_dust/render/backend/render_backend.h>

// RENDER-BACKEND-STAGE-4 (docs/RENDER_BACKEND_ABSTRACTION.md, §7). Empty
// scaffold only -- no real bgfx embedding logic here (§9: separate future
// phase, not this one). Same abstraction class as SdlGpuBackend (explicit
// PSO, command buffer, multi-backend GPU API) -- IRenderBackend maps onto
// it most directly of the 5 candidates once real work starts (§7.1's own
// note); also already evaluated in docs/ENGINE_BENCHMARK.md as "ЗАПОЗИЧИТИ
// (ідея)" (see docs/RENDER_BACKEND_ABSTRACTION.md §0.1). Compiles under
// MD_RENDER_BACKEND=BGFX but is never wired into game_init.cpp's runtime
// backend selection (that's a future phase).
//
// Only BackendName() is overridden here -- every other IRenderBackend
// method has a default "not implemented" body on the base class itself
// (render_backend.h), so this class no longer needs to repeat all 11
// method stubs (docs/SDL_GPU_ECOSYSTEM_2026-09.md, дія №5-поправка).
namespace md::render_backend {

class BgfxBackend final : public IRenderBackend {
public:
    const char* BackendName() const override { return "bgfx (stub)"; }
};

}  // namespace md::render_backend

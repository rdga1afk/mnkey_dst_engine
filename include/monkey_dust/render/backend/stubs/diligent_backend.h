#pragma once
#include <monkey_dust/render/backend/render_backend.h>

// RENDER-BACKEND-STAGE-4 (docs/RENDER_BACKEND_ABSTRACTION.md, §7). Empty
// scaffold only -- no real Diligent Engine embedding logic here (§9:
// separate future phase, not this one). Same abstraction class as
// SdlGpuBackend/bgfx (explicit PSO, command buffer, multi-backend GPU
// API) -- IRenderBackend maps onto it directly (§7.1's own note). Compiles
// under MD_RENDER_BACKEND=DILIGENT but is never wired into game_init.cpp's
// runtime backend selection (that's a future phase).
//
// Only BackendName() is overridden here -- every other IRenderBackend
// method has a default "not implemented" body on the base class itself
// (render_backend.h), so this class no longer needs to repeat all 11
// method stubs (docs/SDL_GPU_ECOSYSTEM_2026-09.md, дія №5-поправка).
namespace md::render_backend {

class DiligentBackend final : public IRenderBackend {
public:
    const char* BackendName() const override { return "Diligent Engine (stub)"; }
};

}  // namespace md::render_backend

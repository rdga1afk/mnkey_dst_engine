#pragma once
#include <monkey_dust/render/backend/render_backend.h>

// RENDER-BACKEND-STAGE-4 (docs/RENDER_BACKEND_ABSTRACTION.md, §7). Empty
// scaffold only -- no real Filament embedding logic here (§9: separate
// future phase, not this one). Second priority of the 5 candidates:
// already influenced this codebase's architecture once before this phase
// (engine/include/monkey_dust/render/terrain_renderer.h's zone-layer LUT
// is a texture, not an SSBO, a 2026-08-09 decision made explicitly to
// narrow the Filament-incompatible compute/SSBO surface -- see that
// file's own doc comment, and docs/RENDER_BACKEND_ABSTRACTION.md §0.1).
// Compiles under MD_RENDER_BACKEND=FILAMENT but is never wired into
// game_init.cpp's runtime backend selection (that's a future phase).
//
// Only BackendName() is overridden here -- every other IRenderBackend
// method has a default "not implemented" body on the base class itself
// (render_backend.h), so this class no longer needs to repeat all 11
// method stubs (docs/SDL_GPU_ECOSYSTEM_2026-09.md, дія №5-поправка).
namespace md::render_backend {

class FilamentBackend final : public IRenderBackend {
public:
    const char* BackendName() const override { return "Filament (stub)"; }
};

}  // namespace md::render_backend

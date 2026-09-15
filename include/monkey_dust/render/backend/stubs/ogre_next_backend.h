#pragma once
#include <monkey_dust/render/backend/render_backend.h>

// RENDER-BACKEND-STAGE-4 (docs/RENDER_BACKEND_ABSTRACTION.md, §7). Empty
// scaffold only -- no real OGRE-Next embedding logic here (§9: separate
// future phase, not this one). Requires a material-conversion layer
// (MaterialDesc -> Hlms, §9) before real work can start, unlike bgfx/
// Diligent's more direct mapping -- but has full production-level
// features (forward+/clustered), unlike the immediate-mode libraries
// (§7.1's own note). Real Kenshi used OGRE3D + the adVantage Chunked-LOD
// plugin specifically for terrain (docs/RE_KENSHI_ADVANTAGE_TERRAIN
// reference), a thematic tie worth noting for planning even though it's
// not a factor in this phase's stub. Compiles under
// MD_RENDER_BACKEND=OGRENEXT but is never wired into game_init.cpp's
// runtime backend selection (that's a future phase).
//
// Only BackendName() is overridden here -- every other IRenderBackend
// method has a default "not implemented" body on the base class itself
// (render_backend.h), so this class no longer needs to repeat all 11
// method stubs (docs/SDL_GPU_ECOSYSTEM_2026-09.md, дія №5-поправка).
namespace md::render_backend {

class OgreNextBackend final : public IRenderBackend {
public:
    const char* BackendName() const override { return "OGRE-Next (stub)"; }
};

}  // namespace md::render_backend

// RENDER-BACKEND-STAGE-4 (docs/RENDER_BACKEND_ABSTRACTION.md, §7.3's own
// completion criterion: "cmake -DMD_RENDER_BACKEND=GODOT compiles the
// corresponding stub without errors, even if runtime doesn't use it").
// A header-only stub class that nothing #includes never gets type-checked
// by the build at all -- this tiny TU forces whichever stub CMake selected
// to actually compile (catching pure-virtual-override mismatches, wrong
// signatures, etc.), without wiring it into any real runtime path. When
// MD_RENDER_BACKEND=SDLGPU (the default, the only real backend), none of
// the branches below match and this file compiles to an empty TU.
#if defined(MD_RENDER_BACKEND_GODOT)
#include <monkey_dust/render/backend/stubs/godot_backend.h>
namespace { md::render_backend::GodotBackend s_compile_check; }
#elif defined(MD_RENDER_BACKEND_FILAMENT)
#include <monkey_dust/render/backend/stubs/filament_backend.h>
namespace { md::render_backend::FilamentBackend s_compile_check; }
#elif defined(MD_RENDER_BACKEND_BGFX)
#include <monkey_dust/render/backend/stubs/bgfx_backend.h>
namespace { md::render_backend::BgfxBackend s_compile_check; }
#elif defined(MD_RENDER_BACKEND_DILIGENT)
#include <monkey_dust/render/backend/stubs/diligent_backend.h>
namespace { md::render_backend::DiligentBackend s_compile_check; }
#elif defined(MD_RENDER_BACKEND_OGRENEXT)
#include <monkey_dust/render/backend/stubs/ogre_next_backend.h>
namespace { md::render_backend::OgreNextBackend s_compile_check; }
#endif

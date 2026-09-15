#pragma once
#include <monkey_dust/render/backend/render_backend.h>

// RENDER-BACKEND-STAGE-4 (docs/RENDER_BACKEND_ABSTRACTION.md, §7). Empty
// scaffold only -- no real LibGodot embedding logic here.
//
// NOT an open candidate, unlike the other 4 stubs in this directory --
// the ONLY one of the 5 actually tried end-to-end, and REJECTED
// (docs/SDL3_TO_LIBGODOT_MIGRATION_AUDIT.md, verdict 2026-08-23). R1-R3
// (compile/link/start on real Intel HD 520, ~3.7-3.9ms draw call) looked
// promising and this file's comment used to call it "highest priority"
// on that basis -- but the full migration was then actually built
// (Phases A-F, godot-cpp/GDExtension transition, terrain streaming,
// game parity, editor porting, all verified working end-to-end in a
// separate worktree) and same-session A/B measurement on the target
// hardware found LibGodot's built-in Forward+ renderer costs ~50% more
// GPU time than this project's own hand-tuned SDL_GPU HAL (25.4-25.9ms
// vs 16.76ms/frame, identical scene; confirmed via perf record/perf
// stat/intel_gpu_top). Root cause is architectural (Forward+ renderer
// cost on Gen9), not a linking/embedding issue -- no fix found.
// SDL3+SDL_GPU stays the permanent render path; the worktree with the
// working migration code was deleted, kept only as this history. Do
// NOT resume work on this backend without a new reason to revisit the
// rejected verdict (see the audit doc's own "what could change this").
//
// Compiles under MD_RENDER_BACKEND=GODOT but is never wired into
// game_init.cpp's runtime backend selection.
//
// Only BackendName() is overridden here -- every other IRenderBackend
// method has a default "not implemented" body on the base class itself
// (render_backend.h), so this class no longer needs to repeat all 11
// method stubs (docs/SDL_GPU_ECOSYSTEM_2026-09.md, дія №5-поправка).
namespace md::render_backend {

class GodotBackend final : public IRenderBackend {
public:
    const char* BackendName() const override { return "LibGodot (stub)"; }
};

}  // namespace md::render_backend

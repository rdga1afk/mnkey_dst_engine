#pragma once

// Platform audio abstraction.
// M5 migration (compile with -DUSE_SDL3) made this the only path; the Raylib
// fallback was removed 2026-08-09 (USE_SDL3=ON has been the only buildable
// configuration, see engine/CMakeLists.txt's FATAL_ERROR gate).
// Rule M-A: only Main.cpp may include this header.

// ── Miniaudio path (M17) — delegates to AudioSystem singleton ────────────────
// MINIAUDIO_IMPLEMENTATION lives in audio_system.cpp (engine static lib).
#  include <monkey_dust/audio/audio_system.h>

   inline void audio_init()     { AudioSystem::Get().Init(); }
   inline void audio_shutdown() { AudioSystem::Get().Shutdown(); }

// ── Frame timing abstraction ─────────────────────────────────────────────────
// SDL_GetTicks() provides delta.
#  include <SDL3/SDL.h>
   namespace _frame {
       inline Uint64& prev() { static Uint64 t = 0; return t; }
       inline float&  dt()   { static float  d = 0.f; return d; }
   }
   // Call ONCE per frame at the START (before game logic).
   // input_begin_frame() is called first; call frame_tick() right after.
   inline void frame_tick() {
       Uint64 now = SDL_GetTicks();
       if (_frame::prev() == 0) _frame::prev() = now;
       _frame::dt() = (float)(now - _frame::prev()) * 0.001f;
       if (_frame::dt() > 0.1f) _frame::dt() = 0.1f;  // cap at 100ms
       _frame::prev() = now;
   }
   inline float frame_dt()  { return _frame::dt(); }
   inline int   frame_fps() { return (_frame::dt() > 0.f) ? (int)(1.f / _frame::dt()) : 0; }

#pragma once
// Platform window + frame lifecycle abstraction.
// Rule M-A: include only from Main.cpp and EditorMain.cpp.

// Pure SDL3 + SDL_GPU window, no OpenGL context and no Raylib.
//
// imgui_init / imgui_new_frame / imgui_render / imgui_shutdown:
//   Available only under #ifdef DEBUG.

#  include <SDL3/SDL.h>
#  include <monkey_dust/render/md_camera.h>
#  include <cstdio>
#  include <monkey_dust/render/gpu_device.h>
#  include <monkey_dust/render/gpu_hal_free_functions.h>

   // Compat no-ops for Raylib calls that may still appear in non-migrated game code.
#  define SetTraceLogLevel(...)  ((void)0)

   namespace _wnd {
       inline SDL_Window*&   ptr()  { static SDL_Window*   w = nullptr; return w; }
       inline SDL_GLContext&  ctx()  { static SDL_GLContext  c = nullptr; return c; }
       inline int&           width()  { static int v = 1280; return v; }
       inline int&           height() { static int v = 720;  return v; }
   }

   // w=0/h=0 → auto-detect from the display this window opens on.
   inline void window_init(int w, int h, const char* title) {
       SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO);

       int init_w = (w > 0) ? w : 1280;
       int init_h = (h > 0) ? h : 720;

       // Pure SDL_GPU (Vulkan/Metal): no OpenGL context.
       // SDL_ClaimWindowForGPUDevice requires the window NOT have an active GL context.
       _wnd::ptr() = SDL_CreateWindow(title, init_w, init_h,
           SDL_WINDOW_RESIZABLE | SDL_WINDOW_MAXIMIZED);
       if (!_wnd::ptr()) {
           fprintf(stderr, "[window] SDL_CreateWindow: %s\n", SDL_GetError());
           return;
       }

       // Auto-detect: resize to usable display area.
       if (w <= 0 || h <= 0) {
           SDL_DisplayID disp = SDL_GetDisplayForWindow(_wnd::ptr());
           SDL_Rect bounds = {};
           if (SDL_GetDisplayUsableBounds(disp, &bounds) &&
               bounds.w > 0 && bounds.h > 0) {
               init_w = bounds.w;
               init_h = bounds.h;
               SDL_SetWindowSize(_wnd::ptr(), init_w, init_h);
           }
       }

       _wnd::width()  = init_w;
       _wnd::height() = init_h;

   }

   inline void window_shutdown() {
       SDL_DestroyWindow(_wnd::ptr());     _wnd::ptr() = nullptr;
       SDL_Quit();
   }

   // Software frame cap of the main loops (game main.cpp, editor main.cpp), #191. Setting: -1 = auto (default), 0 = unlimited, > 0 = that many fps.
   // Auto: 60 while there was input or camera movement in the last 0.5 s (window_note_activity), 30 when idle; the 60 applies from the next frame.
   inline int& _window_fps_setting() { static int v = -1; return v; }
   inline Uint64& _window_last_activity_ns() { static Uint64 v = 0; return v; }
   inline void window_set_fps_cap(int fps) { _window_fps_setting() = fps < -1 ? -1 : fps; }
   inline int  window_fps_setting() { return _window_fps_setting(); }
   inline void window_note_activity() { _window_last_activity_ns() = SDL_GetTicksNS(); }
   inline int& _window_idle_fps() { static int v = 0; return v; }   // deep idle (#207): > 0 = auto cap falls to this many fps after 2 s without input; 0 = off
   inline void window_set_idle_fps(int fps) { _window_idle_fps() = fps < 0 ? 0 : fps; }
   inline int  window_fps_cap() {   // the cap in force now (0 = unlimited)
       const int s = _window_fps_setting();
       if (s != -1) return s;
       const Uint64 idle_ns = SDL_GetTicksNS() - _window_last_activity_ns();
       if (idle_ns < 500000000ULL) return 60;
       return (_window_idle_fps() > 0 && idle_ns > 2000000000ULL) ? _window_idle_fps() : 30;
   }

   // The wait at the top of a frame: sleeps until 1/cap s after the previous frame start. In auto mode it sleeps in 2 ms slices and, when a key / mouse
   // event arrives meanwhile, switches to the 60 fps interval at once (no 33 ms wait before the first frame of an input).
   // True when a queued key / button / wheel / text event or a mouse motion with a non-zero delta is waiting (zero-delta motion is not activity).
   inline bool window_has_input_event() {
       SDL_Event ev[16];
       const int n = SDL_PeepEvents(ev, 16, SDL_PEEKEVENT, SDL_EVENT_KEY_DOWN, SDL_EVENT_MOUSE_WHEEL);
       for (int i = 0; i < n; ++i)
           if (ev[i].type != SDL_EVENT_MOUSE_MOTION || ev[i].motion.xrel != 0.f || ev[i].motion.yrel != 0.f) return true;
       return false;
   }
   inline void window_frame_cap_wait(Uint64& prev_ns) {
       while (prev_ns) {
           if (_window_fps_setting() == -1) {
               SDL_PumpEvents();
               if (window_has_input_event()) window_note_activity();
           }
           const int cap = window_fps_cap();
           if (cap <= 0) break;
           const Uint64 target = 1000000000ULL / (Uint64)cap;
           const Uint64 now = SDL_GetTicksNS();
           if (now - prev_ns >= target) break;
           const Uint64 left = target - (now - prev_ns);
           SDL_DelayNS(left < 2000000ULL ? left : 2000000ULL);
       }
       prev_ns = SDL_GetTicksNS();
   }

   inline void window_set_vsync(int fps) {
       auto& dev = md::GpuDevice::Get();
       if (dev.IsReady()) {
           bool has_vsync   = GpuWindowSupportsPresentMode(dev.SDLDevice(), dev.Window(), SDL_GPU_PRESENTMODE_VSYNC);
           bool has_mailbox = GpuWindowSupportsPresentMode(dev.SDLDevice(), dev.Window(), SDL_GPU_PRESENTMODE_MAILBOX);
           SDL_Log("[vsync] supported: vsync=%d mailbox=%d requested=%s",
                   has_vsync, has_mailbox, fps > 0 ? "vsync" : "no-vsync");
           SDL_GPUPresentMode mode;
           if (fps > 0 && has_vsync) {
               mode = SDL_GPU_PRESENTMODE_VSYNC;
           } else if (!fps && has_mailbox) {
               mode = SDL_GPU_PRESENTMODE_MAILBOX;
           } else {
               mode = SDL_GPU_PRESENTMODE_IMMEDIATE;
           }
           bool ok = GpuSetSwapchainParameters(dev.SDLDevice(), dev.Window(),
               SDL_GPU_SWAPCHAINCOMPOSITION_SDR, mode);
           SDL_Log("[vsync] SetGPUSwapchainParameters mode=%d ok=%d err=%s",
                   (int)mode, ok, ok ? "none" : SDL_GetError());
       }
   }
   inline int  window_get_width()  { return _wnd::width(); }
   inline int  window_get_height() { return _wnd::height(); }
   inline float window_get_time_s() { return (float)(SDL_GetTicks() * 0.001); }

   // terrain-perf-measure (2026-08-12): live resize for the resolution-
   // scaling frame-time test (md.set_window_size). window_begin_frame()
   // re-reads the real size from SDL every frame (SDL_GetWindowSize), and
   // TerrainShadingProjected::EnsureSize already tracks window resizes
   // correctly (diagnosed/fixed 2026-08-02) -- so no extra plumbing needed
   // beyond the resize call itself, EXCEPT: window_init() creates the
   // window with SDL_WINDOW_MAXIMIZED (line ~44), and SDL_SetWindowSize()
   // is a documented no-op on a maximized window on most platforms/WMs --
   // confirmed empirically (2026-08-12): every resize call in a 4-point
   // resolution-scaling test silently did nothing, gbuf_w/gbuf_h stayed
   // at the maximized 1920x1056 the whole time, and the resulting "flat
   // GPU cost vs resolution" data was measuring one single resolution
   // four times, not four different ones. SDL_RestoreWindow() un-maximizes
   // first so the resize actually takes effect.
   inline void window_set_size(int w, int h) {
       if (w <= 0 || h <= 0) return;
       SDL_RestoreWindow(_wnd::ptr());
       SDL_SetWindowSize(_wnd::ptr(), w, h);
   }

   // Sync window size. SDL_GPU: frame clear handled by render pass; no GL calls.
   inline void window_begin_frame() {
       int w = 0, h = 0;
       SDL_GetWindowSize(_wnd::ptr(), &w, &h);
       if (w != _wnd::width() || h != _wnd::height()) {
           _wnd::width()  = w;
           _wnd::height() = h;
       }
   }

   inline void window_begin_3d(const MdCamera& /*cam*/) {
   }
   inline void window_end_3d() {
   }

   // SDL_GPU: frame presented via GpuDevice::Submit(); just pump events.
   inline void window_end_frame() {
       // Events are NOT drained here — they stay in the SDL queue until the next
       // frame's pump (imgui_pump_events or the caller's own SDL_PollEvent loop).
       // Draining here would silently consume events between imgui_new_frame and
       // window_end_frame, causing missed clicks in ImGui.
   }


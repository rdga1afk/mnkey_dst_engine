#pragma once
// Programmatic RenderDoc capture — bypasses two unreliable automation paths:
// the F12 global-hotkey (synthetic/XTest key events don't reach RenderDoc's
// overlay hook in this environment) and `TriggerCapture()`/`renderdoccmd`
// (GitHub baldurk/renderdoc#3255: renderdoccmd is explicitly internal/
// unsupported, and TriggerCapture queues on the *next* present — fragile if
// present timing is unusual). Uses explicit StartFrameCapture/EndFrameCapture
// bracketing around one full frame instead, per the maintainer's documented
// fix for apps without a simple present boundary. No-op when not running
// under RenderDoc (LD_PRELOAD unset) — safe to call always.
// Debug/diagnostic tooling only, not for release builds.

bool RenderDocHook_Init();

// Call from a script/console command to capture the NEXT full frame.
// The capture goes to temp_/captures/<tag>_<time>_frame<N>.rdc (tag defaults to "capture").
void RenderDocHook_ArmCapture(const char* tag = nullptr);

// Call once per frame, immediately before the frame's render work begins.
// If a capture is armed, starts it (StartFrameCapture) and clears the arm flag.
void RenderDocHook_BeginFrame();

// Call once per frame, immediately after the frame's command buffer has been
// submitted/presented. If a capture is in progress, ends it (EndFrameCapture)
// and logs whether the .rdc was written successfully.
void RenderDocHook_EndFrame();

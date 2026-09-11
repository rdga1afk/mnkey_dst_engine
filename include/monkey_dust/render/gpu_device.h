#pragma once
#ifdef MD_SDL_GPU
// ─────────────────────────────────────────────────────────────────────────────
// GpuDevice — SDL_GPU device singleton. Step 0 of SDL_GPU migration.
//
// Manages the SDL_GPUDevice* lifetime and per-frame command buffer flow.
// Call Init() once after window_init(). Falls back gracefully if Vulkan is
// unavailable: IsReady() returns false and the game continues on OpenGL.
//
// Per-frame sequence (Step 9+):
//   auto* cmd  = GpuDevice::Get().AcquireCommandBuffer();
//   auto* swap = GpuDevice::Get().AcquireSwapchainTexture(cmd, &w, &h);
//   // ... record passes ...
//   GpuDevice::Get().Submit(cmd);
// ─────────────────────────────────────────────────────────────────────────────

#include <SDL3/SDL_gpu.h>
#include <cstdint>
#include <mutex>

namespace md {

// Opaque GPU handle type aliases (Granite migration M2 Крок 2 --
// docs/GRANITE_MIGRATION_PLAN_M0_M6.md, docs/GRANITE_M2_OPAQUE_HANDLE_INVENTORY.md).
// Additive only: introduced parallel to the existing raw-SDL-typed API below.
// Zero caller changes in this step -- the 94 files that spell these types in
// their own signatures still use SDL_GPU* directly today. M2 Крок 3 migrates
// them incrementally to these names, group by group (per the inventory doc).
// Currently a zero-cost alias to the raw SDL_GPU pointer since only the
// SDL_GPU backend exists; when a Granite backend lands this block is the
// only place that changes -- already-migrated call sites do not.
using GpuDeviceHandle        = SDL_GPUDevice*;
using GpuCommandBufferHandle = SDL_GPUCommandBuffer*;
using GpuTextureHandle       = SDL_GPUTexture*;
using GpuFenceHandle         = SDL_GPUFence*;

class GpuDevice {
public:
    static GpuDevice& Get();

    // Create SDL_GPUDevice + claim window. Call after window_init().
    // Returns false if no suitable GPU driver found.
    bool       Init(SDL_Window* window);
    void       Shutdown();

    bool           IsReady()    const { return device_ != nullptr; }
    // TEMPORARY raw-SDL bridge (M2 opaque-handle plan, docs/GRANITE_M2_OPAQUE_HANDLE_INVENTORY.md
    // "вимога наперед"): 95 call sites still call this directly instead of
    // holding a GpuDeviceHandle end-to-end. Must disappear from the public
    // API (or gain a real expiry) once M2 Крок 3's caller migration reaches
    // these sites -- same idiom as GpuPassView::FromRaw. Do not add new
    // call sites; route through GpuDeviceHandle-typed parameters instead.
    SDL_GPUDevice* SDLDevice()  const { return device_; }
    SDL_Window*    Window()     const { return window_; }
    const char*    DriverName() const;

    // Per-frame: get a command buffer for recording this frame's GPU work.
    SDL_GPUCommandBuffer* AcquireCommandBuffer();

    // Acquire the swapchain texture to render into. Must be called inside a
    // command buffer acquired with AcquireCommandBuffer().
    SDL_GPUTexture* AcquireSwapchainTexture(SDL_GPUCommandBuffer* cmd,
                                             uint32_t* out_w, uint32_t* out_h);

    // Advance the frame slot (0→1→2→0). Call once per frame before any uploads.
    void AdvanceFrameSlot();
    // Current triple-buffer slot (0..2). Per-frame SSBOs/RingBuffers use this.
    int  FrameSlot() const { return frame_slot_; }

    // Wait for previous frame's fence, then release it.
    // Call once at the top of each frame BEFORE AdvanceFrameSlot.
    void BeginFrame();

    // Submit the command buffer and acquire a fence for next-frame sync.
    void Submit(SDL_GPUCommandBuffer* cmd);

    // Synchronous one-shot fence cycle -- distinct from Submit() above,
    // which is deliberately async (fence released a whole frame late in
    // BeginFrame()). Callers that need this cmd's GPU work done NOW (bake
    // passes, debug dumps, screenshot readback) submit via
    // SubmitAndAcquireFence, then WaitForFence/ReleaseFence themselves in
    // that exact order -- kept as 3 separate 1:1 wrappers, not one fused
    // helper, because real callers interleave real work between the
    // steps (sync-timing instrumentation, error-path cleanup that must
    // run before vs. after the wait) that a fused helper would have to
    // either drop or grow parameters for.
    SDL_GPUFence* SubmitAndAcquireFence(SDL_GPUCommandBuffer* cmd);
    bool          WaitForFence(SDL_GPUFence* fence);
    void          ReleaseFence(SDL_GPUFence* fence);

    // Block until all GPU work on this device has completed. Real,
    // narrow use: a one-shot Init() path needs its uploads visible before
    // treating a resource as ready, and cannot rely on next-frame fencing
    // because there might not be a "next frame" yet (e.g. before the
    // interactive loop's first real render). Not a per-frame call.
    void WaitForIdle();

    // terrain-perf-measure (2026-08-12): opt-in synchronous GPU timing.
    // Normal Submit() is deliberately async (fence released a whole frame
    // late, in BeginFrame() -- lets CPU and GPU overlap). That's exactly
    // why frame_dt()-based measurement is blind to real GPU cost here: at
    // TARGET_FPS=60 the GPU has slack, so the software frame-cap
    // (main.cpp) eats the difference and frame_dt() never moves. When
    // SetSyncTiming(true), Submit() instead waits on THIS frame's fence
    // immediately and times the wait -- LastGpuMs() is real serialized GPU
    // execution time (submit-to-fence-signal), at the cost of killing
    // CPU/GPU pipelining for as long as the flag is on. NOT a shippable
    // mode -- test-only, default off, md.set_gpu_sync_timing(bool).
    void  SetSyncTiming(bool on) { sync_timing_ = on; }
    bool  SyncTiming() const     { return sync_timing_; }
    float LastGpuMs() const      { return last_gpu_ms_; }

    // GEOCLIPMAP Phase 7 frame-time A/B (2026-08-16): a screenshot-pending
    // frame submits via EditorScreenshot_CaptureAndSubmit (editor_screenshot.cpp),
    // NOT this class's own Submit() -- that function must append its own
    // copy-pass + readback to the SAME command buffer before submitting, so
    // it owns the submit+fence-wait itself (SetSyncTiming's Submit() path
    // can't be reused there). Lets that caller report its own
    // already-synchronous fence-wait time here so LastGpuMs() stays
    // meaningful even on frames rendered only because a screenshot was
    // pending (RunScenarioMode/--exec never renders otherwise) -- found via
    // md.get_gpu_ms() reading 0 for every sample despite real renders
    // happening (confirmed via [EditorScreenshot] saved log lines).
    void RecordExternalGpuMs(float ms) { last_gpu_ms_ = ms; }

    // Frame-resource-resize tripwire (RENDER_VS_GRANITE_DEEPSEEK_RESEARCH.md,
    // scene_orchestration topic): resizing a persistent texture (depth,
    // G-buffer, ...) while a command buffer is mid-recording caused a real
    // SIGSEGV inside the Intel Vulkan driver (see npc_render_deferred.cpp's
    // RenderFrame doc comment) -- a still-forming/in-flight command buffer
    // referenced the just-destroyed texture. Correct call sites already run
    // before AcquireCommandBuffer(); this is a regression tripwire for the
    // next one that doesn't. Call before any Shutdown()/Init()/EnsureSize()
    // on a persistent frame resource.
    bool HasActiveCommandBuffer() const { return cmd_buffer_active_; }
    void WarnIfActive(const char* what) const;
    // The screenshot-capture path (tools/editor/editor_screenshot.cpp)
    // submits its own command buffer directly via SDL_GPU, bypassing
    // Submit() above -- it must report completion here itself, mirroring
    // RecordExternalGpuMs's already-established external-caller pattern.
    void NotifyCommandBufferSubmitted() { cmd_buffer_active_ = false; }

private:
    GpuDevice() = default;
    SDL_GPUDevice* device_       = nullptr;
    SDL_Window*    window_       = nullptr;
    int            frame_slot_   = 0;
    SDL_GPUFence*  prev_fence_   = nullptr;
    bool           sync_timing_  = false;
    float          last_gpu_ms_  = 0.f;
    bool           cmd_buffer_active_ = false;
    // GATE-5/W3D-freeze fix (2026-09-12): AcquireCommandBuffer/Submit are
    // called from the main render thread EVERY frame, AND from the editor's
    // WorldEditor3D_SDLGPU loader thread (TerrainRenderer::Init, DDS array
    // upload, PropMesh::LoadGLB -- all route through this same singleton).
    // prev_fence_/cmd_buffer_active_/frame_slot_ were plain, unsynchronized
    // fields -- a real data race, confirmed as the root cause of two prior
    // SIGSEGVs at different call sites (docs/CLAUDE_STATE.md GATE 5,
    // 2026-09-10), which the loader thread was subsequently made fully
    // synchronous to work around (trading the async, non-blocking editor
    // startup for correctness -- the visible ~6.5s "freeze" on editor
    // launch/reload). This mutex is the narrower fix speculated about in
    // that decision's own comment ("a mutex around SDL_GPU calls") --
    // serializes both the C++ bookkeeping AND the underlying
    // SDL_SubmitGPUCommandBufferAndAcquireFence call (Vulkan itself
    // requires external synchronization for concurrent queue submission,
    // independent of this class's own state). `mutable` so the const
    // WarnIfActive() can also take it.
    mutable std::mutex mu_;
};

} // namespace md

#endif // MD_SDL_GPU

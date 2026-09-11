#ifdef MD_SDL_GPU
#include <monkey_dust/render/gpu_device.h>
#include <monkey_dust/render/gpu_frame_timeline.h>
#include <monkey_dust/render/gpu_hal.h>  // MdPipeCache_Shutdown/MdSpvCache_Shutdown
#include <monkey_dust/platform/md_log.h>
#include <SDL3/SDL_gpu.h>
#include <SDL3/SDL_timer.h>  // SDL_GetPerformanceCounter/Frequency -- sync-timing path

namespace md {

GpuDevice& GpuDevice::Get() {
    static GpuDevice instance;
    return instance;
}

bool GpuDevice::Init(SDL_Window* window) {
    // Vulkan validation layers add real per-pipeline compile overhead (confirmed:
    // ~4.5s stall creating the terrain_forward pipeline alone, reproducible every
    // launch) — gated on MD_GPU_VALIDATION, NOT plain DEBUG, since DEBUG is also
    // defined whenever MONKEY_DUST_EDITOR=ON (engine/CMakeLists.txt) regardless of
    // CMAKE_BUILD_TYPE, which would otherwise leave validation on for every normal
    // editor-enabled dev build, not just genuine CMAKE_BUILD_TYPE=Debug sessions.
    // Previously hardcoded `true` unconditionally ("TEMP: validation always on to
    // diagnose GPU crash") and never reverted — restore the real opt-in gate.
#ifdef MD_GPU_VALIDATION
    constexpr bool kDebug = true;
#else
    constexpr bool kDebug = false;
#endif
    device_ = SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_SPIRV, kDebug, NULL);
    if (!device_) {
        MD_LOG(MD_LOG_WARNING, "[GpuDevice] SDL_CreateGPUDevice failed: %s", SDL_GetError());
        return false;
    }
    if (!SDL_ClaimWindowForGPUDevice(device_, window)) {
        MD_LOG(MD_LOG_WARNING, "[GpuDevice] SDL_ClaimWindowForGPUDevice failed: %s", SDL_GetError());
        SDL_DestroyGPUDevice(device_);
        device_ = nullptr;
        return false;
    }
    window_ = window;
    MD_LOG(MD_LOG_INFO, "[GpuDevice] Ready. Driver: %s", SDL_GetGPUDeviceDriver(device_));
    return true;
}

void GpuDevice::Shutdown() {
    if (device_) {
        // 2026-07-25: async uploads (e.g. GpuTexture::InitFromDDSArray)
        // submit a command buffer and release their host-side transfer
        // buffer immediately after, with no fence wait — correct as long
        // as SOME later frame's own sync (BeginFrame's fence wait, or just
        // elapsed wall-clock during normal play) catches up before the
        // buffer's memory is reused. A script that calls md.quit() only a
        // few ticks after a large upload (e.g. the 125-layer/699MB ground
        // texture array) skips all of that — SDL_DestroyGPUDevice below
        // could then run while the GPU copy is still in flight, corrupting
        // the Intel ANV driver's internal state (observed: SIGABRT heap
        // corruption and SIGSEGV in __munmap, both inside libvulkan_intel.so
        // during this exact Shutdown() call, both only under --exec
        // scenarios that quit quickly — never during normal interactive
        // use, which naturally gives every upload time to finish).
        SDL_WaitForGPUIdle(device_);
        // MdPipeCache_Shutdown/MdSpvCache_Shutdown exist and are declared for
        // exactly this purpose (splash_screen.cpp's own comment already
        // assumed "whole cache is freed by MdPipeCache_Shutdown() at exit
        // regardless") but were never actually wired in here -- confirmed via
        // a live SDL_CreateGPU*/SDL_ReleaseGPU* call-count audit
        // (docs/SDLGPU_FENCE_OBJECT_LEAK_BUG.md): 19 GraphicsPipeline created,
        // 0 released, exactly matching this cache's live entry count.
        MdPipeCache_Shutdown();
        MdSpvCache_Shutdown();
        SDL_DestroyGPUDevice(device_);
        device_ = nullptr;
        window_ = nullptr;
        // M1 HAL-closure fix (2026-08-31): prev_fence_ is a handle owned by
        // the device just destroyed above -- leaving it set here is a
        // cross-device use-after-free waiting to happen the next time
        // Init() is called (a fresh SDL_GPUDevice*, but Submit()'s first
        // line would still try to SDL_ReleaseGPUFence() this stale handle
        // against the NEW device). Was latent because nothing outside a
        // single Init-to-process-exit lifetime called Submit() without a
        // full BeginFrame() cycle to drain it first -- surfaced by the GPU
        // unit tests, which each Init()/Shutdown() their own isolated
        // device and call one-shot Init() paths (e.g. TerrainWorldHeightmap
        // ::Init()) that Submit() without ever calling BeginFrame() --
        // confirmed via gdb backtrace: SIGSEGV inside libvulkan_intel.so,
        // called from Submit() from the NEXT test's Init() path.
        prev_fence_        = nullptr;
        cmd_buffer_active_ = false;
        frame_slot_        = 0;
    }
}

const char* GpuDevice::DriverName() const {
    return device_ ? SDL_GetGPUDeviceDriver(device_) : "none";
}

SDL_GPUCommandBuffer* GpuDevice::AcquireCommandBuffer() {
    std::lock_guard<std::mutex> lock(mu_);
    SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(device_);
    if (cmd) cmd_buffer_active_ = true;
    return cmd;
}

void GpuDevice::WarnIfActive(const char* what) const {
    std::lock_guard<std::mutex> lock(mu_);
    if (cmd_buffer_active_) {
        MD_LOG(MD_LOG_WARNING,
               "[GpuDevice] %s called while a command buffer is active -- "
               "resizing/destroying a persistent GPU resource mid-frame can "
               "use-after-free on the Intel Vulkan driver (see "
               "npc_render_deferred.cpp RenderFrame doc comment)", what);
    }
}

SDL_GPUTexture* GpuDevice::AcquireSwapchainTexture(SDL_GPUCommandBuffer* cmd,
                                                    uint32_t* out_w, uint32_t* out_h) {
    SDL_GPUTexture* tex = nullptr;
    SDL_AcquireGPUSwapchainTexture(cmd, window_, &tex, out_w, out_h);
    return tex;
}

void GpuDevice::AdvanceFrameSlot() {
    std::lock_guard<std::mutex> lock(mu_);
    frame_slot_ = (frame_slot_ + 1) % 3;
}

void GpuDevice::BeginFrame() {
    std::lock_guard<std::mutex> lock(mu_);
    if (!prev_fence_ || !device_) return;
    SDL_WaitForGPUFences(device_, true, &prev_fence_, 1);
    SDL_ReleaseGPUFence(device_, prev_fence_);
    prev_fence_ = nullptr;
    // Timeline: fence signaled = GPU finished previous frame.
    GpuFrameTimeline::Get().OnFenceSignaled();
}

void GpuDevice::Submit(SDL_GPUCommandBuffer* cmd) {
    std::lock_guard<std::mutex> lock(mu_);
    // prev_fence_ is shared by every caller of Submit() (the main per-frame
    // render path AND every utility subsystem -- texture upload/terrain
    // streaming/SSBO upload -- that also submits through this same
    // function). If one of those secondary Submit() calls lands after the
    // primary frame's Submit() but before the next BeginFrame() (which is
    // the only other prev_fence_ consumer and DOES wait first), prev_fence_
    // is still set here and must be retired the same way BeginFrame() does
    // it: wait, then release. Releasing without waiting (the previous
    // version of this block) drops the last reference to a fence whose GPU
    // work has NOT finished, returning a genuinely not-yet-signaled fence to
    // SDL3's pool -- confirmed live as the root cause of the intermittent
    // VUID-vkResetFences-pFences-01123 validation error (fence reused/reset
    // while still associated with an incomplete queue submission), see
    // docs/SDLGPU_FENCE_OBJECT_LEAK_BUG.md §9.
    if (prev_fence_) {
        SDL_WaitForGPUFences(device_, true, &prev_fence_, 1);
        SDL_ReleaseGPUFence(device_, prev_fence_);
        prev_fence_ = nullptr;
    }

    if (sync_timing_) {
        // terrain-perf-measure (2026-08-12): serialize on THIS frame's fence
        // instead of deferring to next frame's BeginFrame() -- see header
        // doc comment for why the normal async path is blind to real GPU
        // cost under the TARGET_FPS software cap.
        SDL_GPUFence* f = SDL_SubmitGPUCommandBufferAndAcquireFence(cmd);
        uint64_t t0 = SDL_GetPerformanceCounter();
        if (f) SDL_WaitForGPUFences(device_, true, &f, 1);
        uint64_t t1 = SDL_GetPerformanceCounter();
        uint64_t freq = SDL_GetPerformanceFrequency();
        last_gpu_ms_ = freq ? (float)((double)(t1 - t0) * 1000.0 / (double)freq) : 0.f;
        if (f) SDL_ReleaseGPUFence(device_, f);
        GpuFrameTimeline::Get().OnSubmit();
        cmd_buffer_active_ = false;
        return;
    }

    prev_fence_ = SDL_SubmitGPUCommandBufferAndAcquireFence(cmd);
    // Timeline: record submit timestamp for latency measurement.
    GpuFrameTimeline::Get().OnSubmit();
    cmd_buffer_active_ = false;
}

SDL_GPUFence* GpuDevice::SubmitAndAcquireFence(SDL_GPUCommandBuffer* cmd) {
    // No class state touched here, but the underlying SDL/Vulkan queue
    // submit itself needs external synchronization against Submit()'s own
    // SDL_SubmitGPUCommandBufferAndAcquireFence call from another thread --
    // same mutex, see its doc comment on mu_ (gpu_device.h).
    std::lock_guard<std::mutex> lock(mu_);
    return SDL_SubmitGPUCommandBufferAndAcquireFence(cmd);
}

bool GpuDevice::WaitForFence(SDL_GPUFence* fence) {
    std::lock_guard<std::mutex> lock(mu_);
    if (!fence) return false;
    return SDL_WaitForGPUFences(device_, true, &fence, 1);
}

void GpuDevice::ReleaseFence(SDL_GPUFence* fence) {
    std::lock_guard<std::mutex> lock(mu_);
    if (fence) SDL_ReleaseGPUFence(device_, fence);
}

void GpuDevice::WaitForIdle() {
    std::lock_guard<std::mutex> lock(mu_);
    if (device_) SDL_WaitForGPUIdle(device_);
}

} // namespace md
#endif // MD_SDL_GPU

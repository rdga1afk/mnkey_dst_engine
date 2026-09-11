#pragma once
// Granite migration M2 (docs/GRANITE_MIGRATION_PLAN_M0_M6.md): optional
// Granite Vulkan device, dual-run alongside GpuDevice's SDL_GPU path.
// Empty no-op API when MD_USE_GRANITE is undefined (USE_GRANITE=OFF) --
// engine/ compiles and links with zero Granite dependency in that
// configuration.
struct SDL_Window;

namespace md {

// RENDER-BACKEND-STAGE-6 (docs/GRANITE_IRENDERBACKEND_INTEGRATION.md §2.3).
// Raw Vulkan handles for a tools/-side ImGui-Vulkan bridge to consume
// (ImGui_ImplVulkan_InitInfo's fields are exactly these types) -- void*
// here, not the real Vk* typedefs, so this header never needs
// <vulkan/vulkan.h> (keeps engine/'s public surface dependency-free even
// when MD_USE_GRANITE is off; tools/-side code casts back to the real
// types, which it already has via its own Vulkan/ImGui includes).
struct GraniteVulkanHandles {
    void*    instance                    = nullptr;  // VkInstance
    void*    physical_device             = nullptr;  // VkPhysicalDevice
    void*    device                      = nullptr;  // VkDevice
    void*    queue                       = nullptr;  // VkQueue (graphics)
    unsigned queue_family                = 0;
    int      swapchain_format            = 0;        // VkFormat
    bool     dynamic_rendering_supported = false;
};

// Injection point for per-frame overlay content (ImGui or otherwise) drawn
// via VK_KHR_dynamic_rendering on top of RenderFrameWithOverlay()'s own
// swapchain clear -- vk_command_buffer is a live VkCommandBuffer (void*
// for the same header-dependency reason as GraniteVulkanHandles above),
// already inside an active vkCmdBeginRendering scope when called.
using GraniteOverlayDrawFn = void (*)(void* user, void* vk_command_buffer);

class GraniteBackend {
public:
    static GraniteBackend& Get();

    // True only when compiled with MD_USE_GRANITE (USE_GRANITE=ON at
    // configure time). Init()/RenderEmptyFrame() are no-ops otherwise.
    bool IsBuilt() const;

    // Wraps the SAME SDL_Window the SDL_GPU path already owns (platform/
    // window.h's _wnd::ptr()) -- one window, one event pump, per M3's
    // "два бекенди не можуть ділити одне вікно" constraint resolved by
    // NOT creating a second window, only a second GPU device on it.
    bool Init(SDL_Window* window);
    void Shutdown();
    bool IsReady() const;

    // M2's own exit criterion: an empty frame clears in game/monkey_dust
    // under USE_GRANITE=ON. Teal, matching probes/granite_m2_wsi's and
    // probes/granite_m3_imgui_spike's visual proof color for continuity.
    void RenderEmptyFrame();

    // For a tools/-side ImGui-Vulkan bridge's ONE-TIME ImGui_ImplVulkan_Init()
    // call. Returns a zeroed struct (dynamic_rendering_supported=false) if
    // !IsReady().
    GraniteVulkanHandles GetVulkanHandlesForImGui() const;

    // docs/GRANITE_IRENDERBACKEND_INTEGRATION.md §2.3 -- the permanent form
    // of probes/granite_m3_imgui_dynamic_rendering.cpp's proven approach
    // (VUID-vkCmdDrawIndexed-renderPass-02684 fix, 0 validation errors, live-
    // verified). Owns the full Granite per-frame sequence (begin_frame,
    // swapchain clear pass, PRESENT_SRC_KHR<->ATTACHMENT_OPTIMAL barriers,
    // vkCmdBeginRendering/EndRendering via the device's own function table,
    // submit, end_frame) -- draw_fn is called once, between
    // vkCmdBeginRendering and vkCmdEndRendering, to draw the actual overlay
    // content. Returns false (draw_fn NOT called) if !IsReady() or
    // wsi.begin_frame() reports no work to do this tick (matches
    // RenderEmptyFrame()'s own early-return case).
    bool RenderFrameWithOverlay(GraniteOverlayDrawFn draw_fn, void* user);

    // docs/GRANITE_IRENDERBACKEND_INTEGRATION.md §5 item 5 (screenshot
    // comparison SDL_GPU vs Granite). One-shot request/consume, same shape
    // as tools/editor/editor_screenshot.h's EditorScreenshot_RequestPending/
    // ConsumePending -- but for Granite's own swapchain, which
    // EditorScreenshot's SDL_GPU-based DownloadFromGPUTexture path cannot
    // reach (no SDL_GPU command buffer/texture exists in the frames
    // RenderFrameWithOverlay() renders).
    //
    // RequestScreenshot(): call any time before the next RenderFrameWithOverlay()
    // call. That call will then capture its own fully-composited frame
    // (swapchain clear + draw_fn's overlay content) via an extra copy_image_
    // to_buffer + host-visible readback buffer, tight-packed RGBA8 (Vulkan-
    // side R/B channel order already corrected against swapchain_format --
    // caller gets true RGBA, no format-dependent swap needed unlike
    // EditorScreenshot's own B8G8R8A8 check).
    void RequestScreenshot();

    // Call once, any time after a RenderFrameWithOverlay() that had a
    // pending request. Returns a malloc()'d width*height*4 RGBA8 buffer
    // (caller must free() it) and fills out_w/out_h, or returns nullptr
    // (out_w/out_h left untouched) if no capture is ready yet (request not
    // consumed by a render call yet, or that call failed/had no work to do).
    // One-shot: clears the captured buffer from internal state either way.
    void* ConsumeScreenshotRGBA(unsigned* out_w, unsigned* out_h);
};

} // namespace md

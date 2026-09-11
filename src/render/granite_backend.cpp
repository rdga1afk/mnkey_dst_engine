#include <monkey_dust/render/granite_backend.h>

#ifdef MD_USE_GRANITE

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

#include "wsi.hpp"
#include "context.hpp"
#include "device.hpp"
#include "command_buffer.hpp"
#include "image.hpp"

#include <monkey_dust/platform/md_log.h>

#include <cstdlib>
#include <vector>

namespace md {
namespace {

// Same MdSdlWsiPlatform recipe as probes/granite_m2_wsi_probe.cpp and
// probes/granite_m3_imgui_spike.cpp (both live-verified on this HD 520) --
// wraps the caller-supplied SDL_Window, does not create its own.
class MdSdlWsiPlatform : public Vulkan::WSIPlatform {
public:
    explicit MdSdlWsiPlatform(SDL_Window* window) : window_(window) {}

    VkSurfaceKHR create_surface(VkInstance instance, VkPhysicalDevice) override {
        VkSurfaceKHR surface = VK_NULL_HANDLE;
        if (!SDL_Vulkan_CreateSurface(window_, instance, nullptr, &surface)) {
            MD_LOG(MD_LOG_WARNING, "[GraniteBackend] SDL_Vulkan_CreateSurface failed: %s", SDL_GetError());
            return VK_NULL_HANDLE;
        }
        return surface;
    }

    std::vector<const char*> get_instance_extensions() override {
        Uint32 count = 0;
        char const* const* exts = SDL_Vulkan_GetInstanceExtensions(&count);
        return std::vector<const char*>(exts, exts + count);
    }

    uint32_t get_surface_width() override {
        int w = 0, h = 0;
        SDL_GetWindowSizeInPixels(window_, &w, &h);
        return (uint32_t)w;
    }

    uint32_t get_surface_height() override {
        int w = 0, h = 0;
        SDL_GetWindowSizeInPixels(window_, &w, &h);
        return (uint32_t)h;
    }

    bool alive(Vulkan::WSI&) override { return true; }
    void poll_input() override {}
    void poll_input_async(Granite::InputTrackerHandler*) override {}

private:
    SDL_Window* window_;
};

struct GraniteState {
    MdSdlWsiPlatform* platform = nullptr;
    Vulkan::WSI* wsi = nullptr;
    bool ready = false;
    // docs/GRANITE_IRENDERBACKEND_INTEGRATION.md §5 item 5 screenshot
    // comparison -- one-shot request/consume, mirrors tools/editor/
    // editor_screenshot.cpp's s_pending_path pattern.
    bool screenshot_pending = false;
    void* screenshot_rgba = nullptr;  // malloc()'d, owned until Consume()'d
    unsigned screenshot_w = 0;
    unsigned screenshot_h = 0;
};

GraniteState& State() {
    static GraniteState s;
    return s;
}

} // namespace

GraniteBackend& GraniteBackend::Get() {
    static GraniteBackend inst;
    return inst;
}

bool GraniteBackend::IsBuilt() const { return true; }

bool GraniteBackend::Init(SDL_Window* window) {
    GraniteState& s = State();
    if (s.ready) return true;
    if (!window) {
        MD_LOG(MD_LOG_WARNING, "[GraniteBackend] Init() called with null window");
        return false;
    }

    // Same R1/M2-Крок-3 pitfall confirmed live twice already (probes'
    // documented history): Vulkan::WSI does not call Context::init_loader()
    // itself -- without this, volkGetInstanceProcAddr resolves to 0.
    if (!Vulkan::Context::init_loader((PFN_vkGetInstanceProcAddr)SDL_Vulkan_GetVkGetInstanceProcAddr())) {
        MD_LOG(MD_LOG_WARNING, "[GraniteBackend] Context::init_loader failed");
        return false;
    }

    s.platform = new MdSdlWsiPlatform(window);
    s.wsi = new Vulkan::WSI();
    s.wsi->set_platform(s.platform);

    Vulkan::Context::SystemHandles handles = {};
    if (!s.wsi->init_simple(1, handles)) {
        MD_LOG(MD_LOG_WARNING, "[GraniteBackend] wsi.init_simple failed");
        delete s.wsi;
        delete s.platform;
        s.wsi = nullptr;
        s.platform = nullptr;
        return false;
    }

    s.ready = true;
    return true;
}

void GraniteBackend::Shutdown() {
    GraniteState& s = State();
    if (!s.ready) return;
    // wsi (holds swapchain/surface, X11/xcb-backed) must be destroyed before
    // the caller destroys its SDL_Window -- confirmed live in M2 Крок 3
    // (reverse order segfaults deep in libvulkan_intel.so's
    // xcb_sync_destroy_fence). Caller owns the window and must not destroy
    // it before calling Shutdown().
    delete s.wsi;
    delete s.platform;
    s.wsi = nullptr;
    s.platform = nullptr;
    s.ready = false;
}

bool GraniteBackend::IsReady() const { return State().ready; }

void GraniteBackend::RenderEmptyFrame() {
    GraniteState& s = State();
    if (!s.ready) return;

    if (!s.wsi->begin_frame()) return;

    auto cmd = s.wsi->get_device().request_command_buffer();
    auto rp = s.wsi->get_device().get_swapchain_render_pass(Vulkan::SwapchainRenderPass::ColorOnly);
    rp.clear_color[0].float32[0] = 0.0f;
    rp.clear_color[0].float32[1] = 0.45f;
    rp.clear_color[0].float32[2] = 0.65f;
    rp.clear_color[0].float32[3] = 1.0f;
    cmd->begin_render_pass(rp);
    cmd->end_render_pass();
    s.wsi->get_device().submit(cmd);
    s.wsi->end_frame();
}

GraniteVulkanHandles GraniteBackend::GetVulkanHandlesForImGui() const {
    GraniteVulkanHandles h;
    GraniteState& s = State();
    if (!s.ready) return h;

    Vulkan::Device& dev = s.wsi->get_device();
    const auto& queue_info = dev.get_queue_info();
    h.instance          = (void*)dev.get_instance();
    h.physical_device   = (void*)dev.get_physical_device();
    h.device            = (void*)dev.get_device();
    h.queue             = (void*)queue_info.queues[Vulkan::QUEUE_INDEX_GRAPHICS];
    h.queue_family      = queue_info.family_indices[Vulkan::QUEUE_INDEX_GRAPHICS];
    h.swapchain_format  = (int)dev.get_swapchain_view().get_format();
    h.dynamic_rendering_supported = dev.get_device_features().vk13_features.dynamicRendering != 0;
    return h;
}

bool GraniteBackend::RenderFrameWithOverlay(GraniteOverlayDrawFn draw_fn, void* user) {
    GraniteState& s = State();
    if (!s.ready) return false;
    if (!s.wsi->begin_frame()) return false;

    Vulkan::Device& dev = s.wsi->get_device();
    auto cmd = dev.request_command_buffer();

    // Same clear+dynamic-rendering-overlay structure as RenderEmptyFrame(),
    // but the actual color matches the real editor's own dark-theme
    // background (tools/editor/main.cpp's SDL_GPU clear pass: 0.10/0.10/
    // 0.13) instead of RenderEmptyFrame()'s teal M2 proof-of-life color --
    // this function is real UI-chrome content now (Крок 4), not a demo,
    // and a mismatched clear color would make the SDL_GPU-vs-Granite
    // screenshot comparison (docs/GRANITE_IRENDERBACKEND_INTEGRATION.md §5
    // item 5) misleadingly show a "different" editor at a glance.
    auto rp = dev.get_swapchain_render_pass(Vulkan::SwapchainRenderPass::ColorOnly);
    rp.clear_color[0].float32[0] = 0.10f;
    rp.clear_color[0].float32[1] = 0.10f;
    rp.clear_color[0].float32[2] = 0.13f;
    rp.clear_color[0].float32[3] = 1.0f;
    cmd->begin_render_pass(rp);
    cmd->end_render_pass();

    const Vulkan::Image& swapchain_image = dev.get_swapchain_view().get_image();

    // PRESENT_SRC_KHR -> ATTACHMENT_OPTIMAL, READ|WRITE dst access (the
    // upcoming loadOp=LOAD read needs READ, not just WRITE -- a real
    // SYNC-HAZARD-READ-AFTER-WRITE validation error until this exact fix,
    // see probes/granite_m3_imgui_dynamic_rendering.cpp's own doc comment).
    cmd->image_barrier(swapchain_image,
                        VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_IMAGE_LAYOUT_ATTACHMENT_OPTIMAL,
                        VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                        VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                        VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);

    VkRenderingAttachmentInfoKHR color_attachment = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO_KHR };
    color_attachment.imageView = dev.get_swapchain_view().get_view().view;
    color_attachment.imageLayout = VK_IMAGE_LAYOUT_ATTACHMENT_OPTIMAL;
    color_attachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    color_attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;

    VkRenderingInfoKHR rendering_info = { VK_STRUCTURE_TYPE_RENDERING_INFO_KHR };
    rendering_info.renderArea.extent.width = s.platform->get_surface_width();
    rendering_info.renderArea.extent.height = s.platform->get_surface_height();
    rendering_info.layerCount = 1;
    rendering_info.colorAttachmentCount = 1;
    rendering_info.pColorAttachments = &color_attachment;

    VkCommandBuffer raw_cmd = cmd->get_command_buffer();
    // Raw global vkCmdBeginRenderingKHR/vkCmdEndRenderingKHR resolve through
    // volk's global function-pointer table, populated only for symbols
    // Granite itself calls -- device.get_device_table() gives the real,
    // populated per-device table instead (same fix as the probe).
    const auto& vk_table = dev.get_device_table();
    vk_table.vkCmdBeginRendering(raw_cmd, &rendering_info);
    if (draw_fn) draw_fn(user, (void*)raw_cmd);
    vk_table.vkCmdEndRendering(raw_cmd);

    // docs/GRANITE_IRENDERBACKEND_INTEGRATION.md §5 item 5: capture the
    // fully-composited frame (clear + draw_fn's overlay) that was just
    // drawn, BEFORE transitioning back to PRESENT_SRC_KHR -- an extra
    // ATTACHMENT_OPTIMAL -> TRANSFER_SRC_OPTIMAL -> PRESENT_SRC_KHR detour
    // instead of the plain ATTACHMENT_OPTIMAL -> PRESENT_SRC_KHR transition
    // used every other frame.
    Vulkan::BufferHandle readback;
    uint32_t cap_w = 0, cap_h = 0;
    if (s.screenshot_pending) {
        cap_w = swapchain_image.get_width();
        cap_h = swapchain_image.get_height();

        cmd->image_barrier(swapchain_image,
                            VK_IMAGE_LAYOUT_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                            VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                            VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);

        Vulkan::BufferCreateInfo bi = {};
        bi.domain = Vulkan::BufferDomain::CachedHost;
        bi.size = VkDeviceSize(cap_w) * cap_h * 4;
        bi.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        readback = dev.create_buffer(bi);

        VkBufferImageCopy region = {};
        region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        region.imageSubresource.layerCount = 1;
        region.imageExtent = { cap_w, cap_h, 1 };
        cmd->copy_image_to_buffer(*readback, swapchain_image, 1, &region);

        cmd->image_barrier(swapchain_image,
                            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                            VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT,
                            VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
    } else {
        cmd->image_barrier(swapchain_image,
                            VK_IMAGE_LAYOUT_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                            VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                            VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
    }

    dev.submit(cmd);

    if (s.screenshot_pending) {
        // Synchronous stall -- on-demand action (like EditorScreenshot_
        // CaptureAndSubmit's own fence-wait), not a per-frame cost.
        dev.wait_idle();
        void* mapped = dev.map_host_buffer(*readback, Vulkan::MEMORY_ACCESS_READ_BIT);
        size_t sz = size_t(cap_w) * cap_h * 4;
        void* out = malloc(sz);
        if (out && mapped) {
            // Swapchain format on this HD 520/ANV path is B8G8R8A8_UNORM
            // (matches EditorScreenshot's own SDL_GPU-side check) --
            // swap to true RGBA here so ConsumeScreenshotRGBA()'s output
            // needs no format-dependent handling by the caller.
            const uint8_t* src_px = (const uint8_t*)mapped;
            uint8_t* dst_px = (uint8_t*)out;
            for (size_t i = 0; i < size_t(cap_w) * cap_h; ++i) {
                dst_px[i*4+0] = src_px[i*4+2];
                dst_px[i*4+1] = src_px[i*4+1];
                dst_px[i*4+2] = src_px[i*4+0];
                dst_px[i*4+3] = src_px[i*4+3];
            }
        }
        if (mapped) dev.unmap_host_buffer(*readback, Vulkan::MEMORY_ACCESS_READ_BIT);
        free(s.screenshot_rgba);  // drop any never-consumed previous capture
        s.screenshot_rgba = out;
        s.screenshot_w = cap_w;
        s.screenshot_h = cap_h;
        s.screenshot_pending = false;
    }

    s.wsi->end_frame();
    return true;
}

void GraniteBackend::RequestScreenshot() {
    State().screenshot_pending = true;
}

void* GraniteBackend::ConsumeScreenshotRGBA(unsigned* out_w, unsigned* out_h) {
    GraniteState& s = State();
    if (!s.screenshot_rgba) return nullptr;
    void* out = s.screenshot_rgba;
    *out_w = s.screenshot_w;
    *out_h = s.screenshot_h;
    s.screenshot_rgba = nullptr;
    s.screenshot_w = s.screenshot_h = 0;
    return out;
}

} // namespace md

#else // !MD_USE_GRANITE -- empty TU, zero Granite dependency

namespace md {
GraniteBackend& GraniteBackend::Get() { static GraniteBackend inst; return inst; }
bool GraniteBackend::IsBuilt() const { return false; }
bool GraniteBackend::Init(SDL_Window*) { return false; }
void GraniteBackend::Shutdown() {}
bool GraniteBackend::IsReady() const { return false; }
void GraniteBackend::RenderEmptyFrame() {}
GraniteVulkanHandles GraniteBackend::GetVulkanHandlesForImGui() const { return {}; }
bool GraniteBackend::RenderFrameWithOverlay(GraniteOverlayDrawFn, void*) { return false; }
void GraniteBackend::RequestScreenshot() {}
void* GraniteBackend::ConsumeScreenshotRGBA(unsigned*, unsigned*) { return nullptr; }
} // namespace md

#endif // MD_USE_GRANITE

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

    // Same swapchain clear pass as RenderEmptyFrame() -- the overlay draws
    // ON TOP of it in a separate vkCmdBeginRendering scope below (matches
    // probes/granite_m3_imgui_dynamic_rendering.cpp's proven sequence).
    auto rp = dev.get_swapchain_render_pass(Vulkan::SwapchainRenderPass::ColorOnly);
    rp.clear_color[0].float32[0] = 0.0f;
    rp.clear_color[0].float32[1] = 0.45f;
    rp.clear_color[0].float32[2] = 0.65f;
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

    cmd->image_barrier(swapchain_image,
                        VK_IMAGE_LAYOUT_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                        VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                        VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);

    dev.submit(cmd);
    s.wsi->end_frame();
    return true;
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
} // namespace md

#endif // MD_USE_GRANITE

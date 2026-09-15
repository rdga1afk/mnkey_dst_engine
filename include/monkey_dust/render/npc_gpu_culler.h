#pragma once
// NpcGpuCuller — GPU frustum culling for NPC sprite lists via Vulkan compute.
//
// Workflow (call once per frame in 3D mode):
//   1. Cull(cmd, npc_x[], npc_z[], count, mvp16)
//      → uploads positions, dispatches npc_cull.comp, copies result back
//   2. After GPU submit + sync: IsVisible(i) → bool
//
// Buffer layout (all 64-element, sizing for MAX_NPCS=64):
//   npc_pos_buf   : vec2[64] RO — NPC world (x, z) positions
//   npc_vis_buf   : uint[64] RW — 1=visible, 0=culled (output)
//   npc_dl_buf    : transfer download buffer — CPU-readable result
//
// On Intel HD 520 (unified memory): GPU→CPU readback is ~0 cost.
// Stalls via SDL_WaitForGPUIdle after submit — acceptable for 64 NPCs.
// For >1000 NPCs consider one-frame-latency double-buffered readback.

#ifdef MD_SDL_GPU
#include <SDL3/SDL_gpu.h>
#include <monkey_dust/render/gpu_compute.h>
#include <monkey_dust/render/gpu_static_buffer.h>
#include <cstdint>

namespace md {

class NpcGpuCuller {
public:
    static constexpr int MAX_NPCS = 64;

    static NpcGpuCuller& Get();

    bool Init();
    void Shutdown();
    bool IsReady() const { return ready_; }

    // Upload NPC positions, dispatch compute, readback result (synchronous).
    // Acquires its own command buffer — independent of the render command buffer.
    // mvp16: column-major float[16] of the 3D camera MVP.
    // npc_radius: world-space half-extent for conservative margin (default 0.5).
    // Returns number of visible NPCs.
    int Cull(const float* npc_x, const float* npc_z, int count,
             const float* mvp16, float npc_radius = 0.5f);

    // Query result after Cull(). Valid until next Cull() call.
    bool IsVisible(int idx) const {
        return (idx >= 0 && idx < last_count_) && (results_[idx] != 0u);
    }

    int LastVisibleCount() const { return last_visible_; }

private:
    GpuComputePipeline pipeline_;
    GpuStaticBuffer    pos_buf_;   // NPC positions (RO)
    GpuStaticBuffer    vis_buf_;   // visibility mask (RW)
    SDL_GPUTransferBuffer* upload_buf_ = nullptr;  // CPU→GPU positions
    SDL_GPUTransferBuffer* dl_buf_     = nullptr;  // GPU→CPU result

    uint32_t results_[MAX_NPCS] = {};
    int      last_count_   = 0;
    int      last_visible_ = 0;
    bool     ready_        = false;
};

} // namespace md
#endif // MD_SDL_GPU

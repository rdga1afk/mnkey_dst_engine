#pragma once
#include <SDL3/SDL_gpu.h>
#include <cstdint>
// render-pass-graph-2026-09-03: was relying on a transitive include from
// gpu_texture_pool.h (removed as dead code, 0 call sites -- see
// render_pass_graph.h's own header comment) for md::GpuDeviceHandle below --
// pure luck of Unity-build batch ordering, not a real dependency edge.
// Broke the instant that transitive path was cut; this header genuinely
// uses the type, so it should include it directly.
#include <monkey_dust/render/gpu_device.h>

// ── RenderTier ────────────────────────────────────────────────────────────────
// Gates M27-M32 deferred pipeline features.
// Values intentionally ordered: higher = more capable.
enum class RenderTier : uint8_t {
    Forward      = 0,  // Intel HD 520 (24 EU, 25.6 GB/s): forward only
    Deferred_Low = 1,  // Iris 540, Vega 8: GBuffer + deferred lights, no SSAO
    Deferred_Med = 2,  // Vega 11: + SSAO half-res compute
    Deferred_High= 3,  // Iris Plus 640/650: + eDRAM GBuffer fit (~1.4 TB/s)
};

// PCI-ID based tier lookup (per CLAUDE_SDL_GPU_PREP_AI.md §I).
// vendor: 0x8086=Intel  0x1002=AMD  0x10DE=NVIDIA
// vram_mb: 0 = unknown (uses conservative heuristic)
RenderTier DetectTier(uint16_t vendor_id, uint16_t device_id, uint32_t vram_mb);

// ── RenderTierSystem ──────────────────────────────────────────────────────────
// Singleton. Call Detect() once after GpuDevice::Init().
// All M27-M32 systems call GetTier() to gate their features.
namespace md {

class RenderTierSystem {
public:
    static RenderTierSystem& Get();

    // Auto-detect: sysfs PCI IDs (Linux) → SDL device name → conservative fallback.
    // json_override_path: optional, e.g. "data/render_settings.json".
    // JSON wins over auto-detect if present and valid.
    RenderTier Detect(md::GpuDeviceHandle dev, const char* json_override_path = nullptr);

    // Hard override (e.g. from CLI arg or game settings menu).
    void       SetTier(RenderTier t) { tier_ = t; }
    RenderTier GetTier()       const { return tier_; }

    bool IsDeferred()  const { return tier_ >= RenderTier::Deferred_Low; }
    bool HasSSAO()     const { return tier_ >= RenderTier::Deferred_Med; }
    bool HaseDRAM()    const { return tier_ == RenderTier::Deferred_High; }

    // VBfA frame_buffer_postprocess_basic: simplified post-pass for low tiers.
    // Forward + Deferred_Low → skip bloom/SSAO/DOF; only CAS sharpening.
    bool UseBasicPostprocess() const { return tier_ <= RenderTier::Deferred_Low; }

    // VBfA 32-tier LOD config with O(1) bitmask presence check.
    // LODConfig[N] = {param_a, param_b}; N ≤ 31; presence_mask bit N = tier active.
    struct LodConfig {
        static constexpr int MAX_TIERS = 32;
        struct Tier { uint8_t param_a, param_b; };
        Tier     tiers[MAX_TIERS]  = {};
        uint32_t presence_mask     = 0;

        bool HasTier(int n) const { return n >= 0 && n < MAX_TIERS && ((presence_mask >> n) & 1u); }
        void SetTier(int n, uint8_t a, uint8_t b) {
            if (n < 0 || n >= MAX_TIERS) return;
            tiers[n] = {a, b};
            presence_mask |= 1u << n;
        }
    };

    const char* TierName() const;

private:
    RenderTierSystem() = default;
    RenderTier tier_ = RenderTier::Forward;
};

} // namespace md


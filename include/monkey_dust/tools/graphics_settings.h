#pragma once
#include <cstdint>
#ifdef MD_DYNAMIC_RES
#  include <algorithm>
#endif

enum class GraphicsPreset : uint8_t { Low = 0, Medium = 1, High = 2 };

// ── GraphicsOverlay ───────────────────────────────────────────────────────────
// MD cPostProcessing analog: Director / cutscenes push named overlays.
// Negative field values mean "no override" — base GraphicsSettings value is used.
// id=0 → inactive slot.  priority: higher value wins per field.
struct GraphicsOverlay {
    float   bloom_intensity    = -1.f;  // < 0 = no override
    float   fog_density        = -1.f;  // < 0 = no override
    float   res_scale_override = -1.f;  // < 0 = no override
    uint8_t priority           =  0;
    uint8_t id                 =  0;
    uint8_t _pad[2];
};
static_assert(sizeof(GraphicsOverlay) == 16, "GraphicsOverlay must be 16 bytes");

struct GraphicsSettings {
    static GraphicsSettings& Get() { static GraphicsSettings s; return s; }

    // ── Display ───────────────────────────────────────────────────────────────
    float fog_near     = 1200.f;  // open-world: fog starts at 1200m (past LOD-1 at 600m)
    float fog_far      = 2800.f;  // fully sky at 2800m (hides streaming edge at ~3×500m)
    // Terrain fog (terrain_forward.slang/terrain_pom.slang, 2026-07-12):
    // linear near/far, computed per-draw as terrain_cr_m (RenderQualityConfig,
    // the actual terrain draw distance) * {fog_near_ratio, 1.0} — NOT this
    // fog_near/fog_far pair (kept above for whatever else may reference it).
    // Ties fog_t=1.0 exactly to the draw-distance cutoff so the outermost
    // LOD-crossfade dither band (terrain_cr_m±40m) is hidden in fog for free.
    // History: briefly FOG_EXP2 (RE-confirmed Kenshi density=0.001,
    // re_docs/kenshi/terrain.md "Subsystem 8") — reverted because a flat
    // density with no near offset saturated fog_t≈1.0 across most of a
    // normal ground-level game view (Kenshi's own model derives near/far
    // from a view-distance setting with a narrow ramp; this engine never
    // implemented that part, so the flat density was too aggressive alone).
    float fog_near_ratio = 0.4f;  // tunable without recompiling shaders
    float fog_density  = 0.001f;  // no longer read by terrain shaders; kept in case anything else references it
    float fog_color[3] = {0.38f, 0.58f, 0.82f};  // matches sky clear color
    bool  fog_enabled  = true;

    bool  vsync            = true;
    float resolution_scale = 1.0f;   // 0.5–1.0; 1.0 = native

    // ── Shadows ───────────────────────────────────────────────────────────────
    bool  shadows_enabled  = true;
    bool  soft_shadows     = true;
    int   shadow_cascades  = 3;      // 1/2/3
    float shadow_distance  = 150.f;

    // ── Post-process / IBL ────────────────────────────────────────────────────
    bool  ibl_enabled  = true;
    float ibl_intensity= 1.f;

    // ── Preset application ────────────────────────────────────────────────────
    void ApplyPreset(GraphicsPreset p) {
        switch (p) {
        case GraphicsPreset::Low:
            soft_shadows    = false;
            shadow_cascades = 1;
            shadow_distance = 80.f;
            resolution_scale= 0.75f;
            ibl_enabled     = false;
            break;
        case GraphicsPreset::Medium:
            soft_shadows    = true;
            shadow_cascades = 2;
            shadow_distance = 120.f;
            resolution_scale= 1.0f;
            ibl_enabled     = true;
            break;
        case GraphicsPreset::High:
            soft_shadows    = true;
            shadow_cascades = 3;
            shadow_distance = 150.f;
            resolution_scale= 1.0f;
            ibl_enabled     = true;
            break;
        }
    }

#ifdef MD_DYNAMIC_RES
    // Call once per frame with previous frame's GPU time (ms).
    // Steps resolution_scale down when over-budget, slowly up when under.
    void UpdateDynamicRes(float frame_time_ms) {
        constexpr float kTargetMs = 14.0f;  // 60fps budget with headroom
        constexpr float kStep     = 0.05f;
        constexpr float kMin      = 0.50f;
        constexpr float kMax      = 1.00f;
        if (frame_time_ms > kTargetMs)
            resolution_scale = std::max(kMin, resolution_scale - kStep);
        else if (frame_time_ms < kTargetMs * 0.85f)
            resolution_scale = std::min(kMax, resolution_scale + kStep * 0.5f);
    }
#endif

    // ── Overlay stack (MD cPostProcessing) ───────────────────────────────
    static constexpr int MAX_OVERLAYS = 4;

    // Add or replace an overlay by id. id=0 is reserved (inactive).
    void PushOverlay(const GraphicsOverlay& ov) noexcept {
        if (!ov.id) return;
        for (int i = 0; i < MAX_OVERLAYS; ++i) {
            if (overlays_[i].id == ov.id || overlays_[i].id == 0) {
                overlays_[i] = ov;
                return;
            }
        }
        // No free slot: evict lowest-priority entry.
        int lo = 0;
        for (int i = 1; i < MAX_OVERLAYS; ++i)
            if (overlays_[i].priority < overlays_[lo].priority) lo = i;
        overlays_[lo] = ov;
    }

    // Remove overlay by id.
    void PopOverlay(uint8_t id) noexcept {
        for (int i = 0; i < MAX_OVERLAYS; ++i)
            if (overlays_[i].id == id) overlays_[i] = GraphicsOverlay{};
    }

    // Returns a copy of the base settings with all active overlays applied.
    // Highest-priority overlay wins per field. Renderer calls this once per frame.
    GraphicsSettings ComputeFinal() const noexcept {
        GraphicsSettings out(*this);
        for (int i = 0; i < MAX_OVERLAYS; ++i) {
            const GraphicsOverlay& ov = overlays_[i];
            if (!ov.id) continue;
            // Find current best priority for each field and apply if higher.
            // Simple scan: last writer wins for equal priority (acceptable).
            if (ov.res_scale_override >= 0.f) {
                bool apply = true;
                for (int j = 0; j < MAX_OVERLAYS; ++j)
                    if (j != i && overlays_[j].id && overlays_[j].res_scale_override >= 0.f
                        && overlays_[j].priority > ov.priority) { apply = false; break; }
                if (apply) out.resolution_scale = ov.res_scale_override;
            }
            if (ov.fog_density >= 0.f) {
                bool apply = true;
                for (int j = 0; j < MAX_OVERLAYS; ++j)
                    if (j != i && overlays_[j].id && overlays_[j].fog_density >= 0.f
                        && overlays_[j].priority > ov.priority) { apply = false; break; }
                if (apply) out.fog_near = fog_near * (1.f - ov.fog_density * 0.5f);
            }
        }
        return out;
    }

private:
    GraphicsSettings() = default;
    GraphicsOverlay overlays_[MAX_OVERLAYS] = {};
};

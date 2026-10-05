#pragma once
#ifdef MD_SDL_GPU
#include <monkey_dust/render/ssbo.h>

// ── AmbientProbe ──────────────────────────────────────────────────────────────
// 128-byte GPU-side probe (aligned to 128 for SSBO std430 array).
// Stores L1 spherical harmonics (4 coefficients per channel — cheap to eval).
// approximates indirect + emissive light.
//
// SH layout: sh_*[0]=L0(constant)  sh_*[1]=L1x  sh_*[2]=L1y  sh_*[3]=L1z
// Shader eval (deferred_lighting.frag):
//   irr.r = sh_r[0] + sh_r[1]*n.x + sh_r[2]*n.y + sh_r[3]*n.z
//   Uses remaining sh_*[4..8] for L2 terms (set to 0 until offline baker added).
struct AmbientProbe {
    float pos_x, pos_y, pos_z;  // world-space probe center
    float radius;                // influence radius (0 = global fallback)
    float sh_r[9];               // L0+L1+L2 SH coefficients for R channel
    float sh_g[9];               // G channel
    float sh_b[9];               // B channel
    float pad[1];                // → sizeof == 128
};
static_assert(sizeof(AmbientProbe) == 128, "AmbientProbe must be 128 bytes");

// ── AmbientProbeSystem ────────────────────────────────────────────────────────
// Singleton. Manages probe array + SSBO upload.
// SSBO binding=8 (after shadow_indirect=7, before SSAO=9).
//
// Typical frame:
//   Init()               — once after GpuDevice::Init()
//   PlaceProbe(...)      — at scene load
//   SetAmbientColor(...) — set L0 term (uniform sky color)
//   SetSkyLight(...)     — set L1 term (directional sky contribution)
//   Upload()             — after any probe change, before draw
//
// Shader: set=1, binding=0 AmbientProbeBuf readonly buffer (M28 deferred pass).
// RADIOSITY constants from AI:
//   deferred_emissive_scale = 1.4875  (apply in shader)
//   deferred_emissive_exp   = 0.5635  (apply in shader)

class AmbientProbeSystem {
public:
    static constexpr int MAX_PROBES  = 64;
    static constexpr int SSBO_BINDING = 8;

    static AmbientProbeSystem& Get();

    // Create SSBO (MAX_PROBES × 128 = 8 KB). Call after GpuDevice::Init().
    void Init();

    // Add a probe at world position with given radius. Returns probe index or -1.
    int  PlaceProbe(float x, float y, float z, float radius);

    // Set L0 SH coefficient (uniform ambient color) for probe[idx].
    // Call after PlaceProbe; then Upload().
    void SetAmbientColor(int idx, float r, float g, float b);

    // Add L1 SH contribution: directional sky light from (dx,dy,dz) with color (r,g,b).
    // Accumulates into probe[idx].sh_*[1..3].
    void SetSkyLight(int idx, float dx, float dy, float dz, float r, float g, float b);

    // Upload probes_ to GPU SSBO. One staging copy per call.
    void Upload();
    // Same data, as a copy pass INSIDE `cmd` (slotted SSBO, no extra submit). Must be called while no render pass is open.
    void UploadInCmd(md::GpuCommandBufferHandle cmd);

    // Remove all probes (keeps SSBO allocated).
    void Clear();

    void Shutdown();

    int              Count()  const { return count_; }
    const AmbientProbe& At(int i) const { return probes_[i]; }
    SSBO&            GetSSBO() { return ssbo_; }

private:
    AmbientProbeSystem() = default;

    AmbientProbe probes_[MAX_PROBES] = {};
    int          count_              = 0;
    SSBO         ssbo_;
};

#endif // MD_SDL_GPU

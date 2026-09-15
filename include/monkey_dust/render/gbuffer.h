#pragma once
#ifdef MD_SDL_GPU
#include <SDL3/SDL_gpu.h>
#include <monkey_dust/render/gpu_pipeline.h>
// ── GBuffer ───────────────────────────────────────────────────────────────────
// Compact 2-RT deferred G-Buffer (8 bytes/pixel at 720p = 7.4 MB).
//
// RT0 RGBA8:              albedo(RGB) + roughness(A)
// RT1 R16G16B16A16_SNORM: oct-normal(RG) + metallic(B) + flags(A)  [VBfA packssdw]
//   flags: bit0=receive_shadow  bit1=emissive  bit2=vegetation
//   SNORM normals: 2× precision vs RGBA8, no UNORM remap in shader needed.
// Depth: D32_FLOAT_S8_UINT
//
// Usage:
//   Init(dev, w, h)         — once after window creation
//   Begin(cmd) → pass*      — start geometry pass; draw into pass
//   End()                   — end geometry pass; RTs ready for sampling
//   RT0()/RT1()/Depth()     — sample in deferred lighting passes (M29-M32)
//   Sampler()               — NEAREST + CLAMP_TO_EDGE for GBuffer reads
//
// Gate: only created when RenderTierSystem::Get().IsDeferred() == true.

class GBuffer {
public:
    void Init(md::GpuDeviceHandle dev, int w, int h);
    void Shutdown();

    // Begin geometry pass with CLEAR on both color RTs + depth.
    // Returns the SDL_GPURenderPass* for the caller to bind pipeline + draw.
    SDL_GPURenderPass* Begin(md::GpuCommandBufferHandle cmd);
    void               End();

    md::GpuTextureHandle RT0()    const { return rt0_;    }  // albedo + roughness
    md::GpuTextureHandle RT1()    const { return rt1_;    }  // oct-normal + metallic
    md::GpuTextureHandle Depth()  const { return depth_;  }
    SDL_GPUSampler* Sampler()const { return sampler_;}
    int Width()  const { return w_; }
    int Height() const { return h_; }
    bool IsReady() const { return rt0_ != nullptr; }

private:
    md::GpuDeviceHandle     dev_     = nullptr;
    md::GpuTextureHandle    rt0_     = nullptr;
    md::GpuTextureHandle    rt1_     = nullptr;
    md::GpuTextureHandle    depth_   = nullptr;
    SDL_GPUSampler*    sampler_ = nullptr;
    GpuCommandBuffer   cb_; // owns the pass across the Begin()/draw/End() span
    int w_ = 0, h_ = 0;
};

// ── GBufferSystem ─────────────────────────────────────────────────────────────
// Singleton wrapper. Manages the scene GBuffer lifecycle.
// IsEnabled() gates the entire deferred path (M28-M32).
namespace md {

class GBufferSystem {
public:
    static GBufferSystem& Get();

    // Creates GBuffer only if RenderTierSystem::Get().IsDeferred().
    // w/h: render resolution (typically 1280×720).
    void Init(md::GpuDeviceHandle dev, int w, int h);
    void Shutdown();

    bool     IsEnabled() const { return enabled_; }
    GBuffer& GetGBuffer()      { return gbuf_; }

private:
    GBufferSystem() = default;
    GBuffer gbuf_;
    bool    enabled_ = false;
};

} // namespace md

#endif // MD_SDL_GPU

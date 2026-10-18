#pragma once
// RdDevice — GPU device capability registry + shared dummy resources.
// Detects Intel ANV limits at Init() and exposes conservative safe defaults.
// All render code should clamp to Caps() before creating pipelines/shaders.
#include <SDL3/SDL_gpu.h>
#include <monkey_dust/render/gpu_device.h>

namespace md {

struct RdDeviceCaps {
    int  max_frag_samplers     = 3;  // Intel ANV HD 520: binding=3 fails silently → cap 3
    int  max_vert_storage_bufs = 4;  // Intel ANV HD 520: 5th storage buf → silent fail
    bool is_intel_anv          = false;
};

class RdDevice {
public:
    static RdDevice& Get() { static RdDevice inst; return inst; }

    void Init(md::GpuDeviceHandle dev);
    void Shutdown();

    const RdDeviceCaps&          Caps()         const { return caps_; }
    md::GpuTextureHandle              DummyTex()     const { return dummy_tex_; }
    SDL_GPUSampler*              DummySamp()    const { return dummy_samp_; }
    SDL_GPUTextureSamplerBinding DummyBinding() const { return {dummy_tex_, dummy_samp_}; }

private:
    RdDevice() = default;
    void BuildDummyResources(md::GpuDeviceHandle dev);

    RdDeviceCaps    caps_;
    md::GpuDeviceHandle  dev_       = nullptr;
    md::GpuTextureHandle dummy_tex_  = nullptr;
    SDL_GPUSampler* dummy_samp_ = nullptr;
};

} // namespace md

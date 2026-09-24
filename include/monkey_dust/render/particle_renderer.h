#pragma once
#include <monkey_dust/render/particle_soa.h>
#include <monkey_dust/render/gpu_pipeline.h>
#include <monkey_dust/platform/math_types.h>


// ParticleRenderer — point-sprite particle system.
// Migrated to GpuPipeline + GpuVertexBuffer + GpuCommandBuffer (Action 3).
class ParticleRenderer {
public:
    static ParticleRenderer& Get() {
        static ParticleRenderer inst;
        return inst;
    }

    void Init();
    void Shutdown();

#ifdef MD_SDL_GPU
    // Call before the render pass (does map→buildVertices→unmap→copy-pass upload).
    // Returns vertex count (0 = nothing to draw).
    int  PrepareSDLGPU(md::GpuCommandBufferHandle cmd, Vec3 cam_pos);
    // Call inside an active render pass.
    void DrawSDLGPU(SDL_GPURenderPass* rp, md::GpuCommandBufferHandle cmd,
                    int count, Mat4 vp, Vec3 cam_pos);
#endif

private:
    ParticleRenderer() = default;

    GpuPipeline      pipeline_;
    GpuVertexBuffer  vbuf_;
};


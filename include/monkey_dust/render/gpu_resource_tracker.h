#pragma once
#include <atomic>

// GpuResourceTracker — live-count of GPU resources created through the
// public HAL wrapper classes (GpuStaticBuffer, GpuTexture, GpuPipeline,
// GpuComputePipeline, GpuDepthTexture). Always-on (atomic increments are
// negligible next to the actual GPU resource creation cost) — no build flag.
//
// Deliberately scoped to these 5 HAL classes' Init/Create + Shutdown/Destroy
// call sites, NOT every raw SDL_CreateGPU*/SDL_ReleaseGPU* call across the
// ~17 internal renderer subsystem files (bloom/ssao/gbuffer/
// etc.) that call SDL_GPU directly for their own resource management —
// retrofitting those ~100 call sites would be a much larger, riskier change.
// This catches leaks in NEW code using the public HAL surface, which is
// what a stress test (tests/stress_gpu_resource_cycle.cpp) exercises.
namespace md {

class GpuResourceTracker {
public:
    static GpuResourceTracker& Get() {
        static GpuResourceTracker inst;
        return inst;
    }

    void OnBufferCreate()          { buffers_.fetch_add(1, std::memory_order_relaxed); }
    void OnBufferDestroy()         { buffers_.fetch_sub(1, std::memory_order_relaxed); }
    void OnTextureCreate()         { textures_.fetch_add(1, std::memory_order_relaxed); }
    void OnTextureDestroy()        { textures_.fetch_sub(1, std::memory_order_relaxed); }
    void OnPipelineCreate()        { pipelines_.fetch_add(1, std::memory_order_relaxed); }
    void OnPipelineDestroy()       { pipelines_.fetch_sub(1, std::memory_order_relaxed); }
    void OnComputePipelineCreate() { compute_pipelines_.fetch_add(1, std::memory_order_relaxed); }
    void OnComputePipelineDestroy(){ compute_pipelines_.fetch_sub(1, std::memory_order_relaxed); }
    void OnDepthTextureCreate()    { depth_textures_.fetch_add(1, std::memory_order_relaxed); }
    void OnDepthTextureDestroy()   { depth_textures_.fetch_sub(1, std::memory_order_relaxed); }

    int LiveBuffers()          const { return buffers_.load(std::memory_order_relaxed); }
    int LiveTextures()         const { return textures_.load(std::memory_order_relaxed); }
    int LivePipelines()        const { return pipelines_.load(std::memory_order_relaxed); }
    int LiveComputePipelines() const { return compute_pipelines_.load(std::memory_order_relaxed); }
    int LiveDepthTextures()    const { return depth_textures_.load(std::memory_order_relaxed); }

    // Sum of all 5 counters — 0 means no leaks across the tracked HAL surface.
    int LiveTotal() const {
        return LiveBuffers() + LiveTextures() + LivePipelines()
             + LiveComputePipelines() + LiveDepthTextures();
    }

private:
    GpuResourceTracker() = default;

    std::atomic<int> buffers_{0};
    std::atomic<int> textures_{0};
    std::atomic<int> pipelines_{0};
    std::atomic<int> compute_pipelines_{0};
    std::atomic<int> depth_textures_{0};
};

}  // namespace md

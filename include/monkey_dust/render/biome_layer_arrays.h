#pragma once
// GPU side of the per-biome ground layer arrays (ground design A): for a biome, one diffuse and one normal texture array of six layers
// (base, slope, cliff, grass, dirt, road) loaded from the install's DDS files, kept under a memory budget by BiomeLayerCache.
#include <monkey_dust/render/biome_layer_cache.h>
#include <monkey_dust/render/gpu_sampler_texture.h>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace md {

struct BiomeLayerSet { GpuTexture diffuse, normal; };

class BiomeLayerArrays {
public:
    BiomeLayerArrays();
    void SetBudgetBytes(uint64_t b) { cache_.SetBudgetBytes(b); }
    // The six-layer arrays of `biome` (index of BiomeRegistry), built on first use; nullptr when a file is missing. Valid until a later Acquire evicts the biome.
    // tier 0..3 = the first 0 / 2 / 4 / 6 mip levels dropped (distance tiers: a far group only reads the tail of the chain); each (biome, tier) is its own entry.
    const BiomeLayerSet* Acquire(int biome, uint64_t frame, int tier = 0) { return static_cast<const BiomeLayerSet*>(cache_.Acquire(biome * 8 + tier, frame)); }
    // Asynchronous use (the game): Peek = resident or nullptr; Request queues the build on the worker thread (file reads, copy, GPU upload and the fence wait all happen there);
    // Poll (once per frame, before the Peeks) moves the finished arrays into the cache. A failed build is remembered and not requested again.
    const BiomeLayerSet* Peek(int biome, uint64_t frame, int tier) { return static_cast<const BiomeLayerSet*>(cache_.Peek(biome * 8 + tier, frame)); }
    void Request(int biome, int tier);
    void Poll(uint64_t frame);
    void SetBuildGapMs(int ms) { gap_ms_ = ms; }   // pause of the worker after each finished build
    int Pending() const;
    const BiomeLayerCache& Cache() const { return cache_; }
    void Clear() { cache_.Clear(); }
    ~BiomeLayerArrays();
private:
    void Worker();
    BiomeLayerCache cache_;
    struct Done { int key; BiomeLayerSet* set; uint64_t bytes; };
    mutable std::mutex mu_; std::condition_variable cv_; std::deque<int> queue_; std::vector<Done> done_; std::vector<int> known_;   // known_: keys requested (queued, running, done or failed)
    std::thread worker_; bool stop_ = false, running_ = false; int in_flight_ = 0; int gap_ms_ = 0;
};

}   // namespace md

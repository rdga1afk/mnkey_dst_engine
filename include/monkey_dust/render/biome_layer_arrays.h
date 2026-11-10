#pragma once
// GPU side of the per-biome ground layer arrays (ground design A): for a biome, one diffuse and one normal texture array of six layers
// (base, slope, cliff, grass, dirt, road) loaded from the install's DDS files, kept under a memory budget by BiomeLayerCache.
#include <monkey_dust/render/biome_layer_cache.h>
#include <monkey_dust/render/gpu_sampler_texture.h>
#include <memory>

namespace md {

struct BiomeLayerSet { GpuTexture diffuse, normal; };

class BiomeLayerArrays {
public:
    BiomeLayerArrays();
    void SetBudgetBytes(uint64_t b) { cache_.SetBudgetBytes(b); }
    // The six-layer arrays of `biome` (index of BiomeRegistry), built on first use; nullptr when a file is missing. Valid until a later Acquire evicts the biome.
    // tier 0..3 = the first 0 / 2 / 4 / 6 mip levels dropped (distance tiers: a far group only reads the tail of the chain); each (biome, tier) is its own entry.
    const BiomeLayerSet* Acquire(int biome, uint64_t frame, int tier = 0) { return static_cast<const BiomeLayerSet*>(cache_.Acquire(biome * 8 + tier, frame)); }
    const BiomeLayerCache& Cache() const { return cache_; }
    void Clear() { cache_.Clear(); }
private:
    BiomeLayerCache cache_;
};

}   // namespace md

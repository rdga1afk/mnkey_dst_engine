#pragma once
// Per-biome ground layer arrays for the original terrain fragment entry (ground design A): the original reads its six layers by literal index from one
// array per biome (base, slope, cliff, grass, dirt, road), diffuse and normal. About 50 MB per biome in the install's formats, so only the biomes in use
// stay resident: a memory budget with least-recently-used eviction. The arrays are built by the injected functions (GPU upload in the game, counters in the test).
#include <cstdint>
#include <functional>
#include <vector>

namespace md {

class BiomeLayerCache {
public:
    struct Built { void* handle = nullptr; uint64_t bytes = 0; };
    using BuildFn = std::function<Built(int biome)>;        // handle == nullptr: the biome cannot be built
    using FreeFn = std::function<void(void* handle)>;
    BiomeLayerCache(BuildFn build, FreeFn release) : build_(std::move(build)), free_(std::move(release)) {}
    ~BiomeLayerCache() { Clear(); }

    void SetBudgetBytes(uint64_t b) { budget_ = b; }
    // The arrays of `biome` (built when absent), marked used in `frame`. Biomes used in this frame are never evicted; others are evicted oldest first
    // while the resident size is over the budget. nullptr when the biome cannot be built.
    void* Acquire(int biome, uint64_t frame);
    void Clear();

    int Resident() const { return (int)items_.size(); }
    uint64_t ResidentBytes() const { return bytes_; }
    uint64_t Builds() const { return builds_; }
    uint64_t Evictions() const { return evictions_; }
    bool Has(int biome) const;

private:
    struct Item { int biome; void* handle; uint64_t bytes, last_use; };
    std::vector<Item> items_;
    BuildFn build_; FreeFn free_;
    uint64_t budget_ = 256ull << 20, bytes_ = 0, builds_ = 0, evictions_ = 0;
};

}   // namespace md

#include <monkey_dust/render/biome_layer_cache.h>

namespace md {

bool BiomeLayerCache::Has(int biome) const { for (const Item& i : items_) if (i.biome == biome) return true; return false; }

void BiomeLayerCache::Evict(uint64_t frame) {
    while (bytes_ > budget_) {   // evict the least recently used biome that this frame does not use
        int victim = -1;
        for (int k = 0; k < (int)items_.size(); ++k) if (items_[k].last_use < frame && (victim < 0 || items_[k].last_use < items_[victim].last_use)) victim = k;
        if (victim < 0) break;
        free_(items_[victim].handle); bytes_ -= items_[victim].bytes; ++evictions_;
        items_.erase(items_.begin() + victim);
    }
}

void* BiomeLayerCache::Peek(int biome, uint64_t frame) {
    for (Item& i : items_) if (i.biome == biome) { i.last_use = frame; return i.handle; }
    return nullptr;
}

void BiomeLayerCache::Insert(int biome, void* handle, uint64_t bytes, uint64_t frame) {
    items_.push_back({ biome, handle, bytes, frame });
    bytes_ += bytes; ++builds_;
    Evict(frame);
}

void* BiomeLayerCache::Acquire(int biome, uint64_t frame) {
    if (void* h = Peek(biome, frame)) return h;
    Built b = build_(biome);
    if (!b.handle) return nullptr;
    Insert(biome, b.handle, b.bytes, frame);
    return b.handle;
}

void BiomeLayerCache::Clear() { for (Item& i : items_) free_(i.handle); items_.clear(); bytes_ = 0; }

}   // namespace md

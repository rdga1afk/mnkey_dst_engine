#include <monkey_dust/render/asset_cache.h>
#include <cstdio>
#include <cstring>

AssetCache& AssetCache::Get() { static AssetCache inst; return inst; }

AssetCache::Entry* AssetCache::Find(const char* path) {
    for (auto& e : entries_) if (e.used && strcmp(e.path, path) == 0) return &e;
    return nullptr;
}

const AssetCache::Entry* AssetCache::Find(const char* path) const {
    for (auto& e : entries_) if (e.used && strcmp(e.path, path) == 0) return &e;
    return nullptr;
}


GpuTexture* AssetCache::GetOrLoadDDS(const char* path, const GpuSamplerDesc& s) {
    if (Entry* e = Find(path)) { e->refcount++; return &e->tex; }
    for (auto& slot : entries_) {
        if (slot.used) continue;
        if (!slot.tex.InitFromDDSArray(&path, 1, s)) {
            fprintf(stderr, "[AssetCache] failed to load DDS %s\n", path);
            return nullptr;
        }
        snprintf(slot.path, PATH_LEN, "%s", path);
        slot.used     = true;
        slot.refcount = 1;
        return &slot.tex;
    }
    fprintf(stderr, "[AssetCache] MAX_ENTRIES (%d) exceeded, cannot load %s\n", MAX_ENTRIES, path);
    return nullptr;
}

void AssetCache::Release(const char* path) {
    Entry* e = Find(path);
    if (!e) return;
    if (--e->refcount <= 0) {
        e->tex.Shutdown();
        e->used     = false;
        e->path[0]  = '\0';
    }
}


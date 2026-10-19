#pragma once
// AssetCache — minimal refcounted GPU texture cache, keyed by file path
// (ARCHITECTURE_IDEAS.md #2). Generalizes the pattern PropTexShared
// implemented ad hoc for exactly two hardcoded DDS textures — any renderer
// that needs a shared, deduplicated GPU texture should reach for this
// instead of writing another one-off *TexShared singleton (that ad hoc fix
// was needed because no general cache existed; this is the general form).
//
// Fixed array (MAX_ENTRIES), no std::map — matches project convention;
// loads only happen at init time, never per-frame.
#include <monkey_dust/render/gpu_sampler_texture.h>
class AssetCache {
public:
    static AssetCache& Get();

    static constexpr int MAX_ENTRIES = 64;
    static constexpr int PATH_LEN    = 160;


    // Same dedup/refcount behaviour, loading via InitFromDDSArray(&path, 1, s)
    // — the single-file-as-1-layer-array form PropTexShared uses for Kenshi
    // DDS assets (InitFromFile does not handle compressed DDS).
    GpuTexture* GetOrLoadDDS(const char* path, const GpuSamplerDesc& s = {});

    // Decrements refcount; frees the GPU texture and slot when it reaches 0.
    void Release(const char* path);


private:
    AssetCache() = default;
    struct Entry {
        char       path[PATH_LEN] = {};
        GpuTexture tex;
        int        refcount = 0;
        bool       used     = false;
    };
    Entry entries_[MAX_ENTRIES] = {};

    Entry*       Find(const char* path);
    const Entry* Find(const char* path) const;
};

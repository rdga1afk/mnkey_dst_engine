#pragma once
// PropTexShared — the two real Kenshi diffuse textures used by every prop
// and clutter mesh (rocks + vegetation). Loaded ONCE and shared across all
// PropRenderer instances — the vegetation atlas alone is
// 4096x4096 BC3 with 13 mips (~11MB); loading it per-PropRenderer (there are
// 6+ instances: props, rock_b, rock_c, veg_yucca, veg_shrub, veg_dtree) would
// waste tens of MB of VRAM for identical data.
//
// layer 0 = tex_rock (GenericRockTexture_DIF.dds, tiled — real Kenshi rock
//           diffuse shared by every rock/formation mesh).
// layer 1 = tex_veg  (Trees&VegAtlas01.dds — real Kenshi shared foliage
//           atlas; each vegetation mesh's baked UV already points at its own
//           sub-rectangle, exactly as authored in the original game).
//
// Backed by AssetCache (ARCHITECTURE_IDEAS.md #2) — this class now only
// holds the two GpuTexture* it looked up there; the actual load/dedup/
// refcount logic lives in the general cache, not here.
#include <monkey_dust/render/gpu_sampler_texture.h>
class PropTexShared {
public:
    static PropTexShared& Get();

    // Idempotent — safe to call from every PropRenderer::Init().
    bool Init();

    GpuTexture* tex_rock = nullptr;
    GpuTexture* tex_veg  = nullptr;
    bool        ready    = false;
};

#pragma once
#include <monkey_dust/render/gpu_device.h>
#include <monkey_dust/render/gpu_sampler_texture.h>
#include <monkey_dust/world/terrain_chunk.h>

// TerrainRenderer — shared ground-texturing RESOURCE MANAGER for
// TerrainPatchRenderer/Granite (task terrain-dedup, 2026-07-29). Despite
// the name, this class no longer draws anything itself: its own draw
// pipeline (terrain_forward.vert/frag) and Draw/DrawRaw/BeginRawBatch/
// DrawRawChunk/SetBatch* methods were verified this session to have ZERO
// callers left anywhere in game/ or tools/editor/ (grepped both) — dead
// weight from before the Granite migration (Phase 8) replaced ALL visual
// terrain rendering. What's still genuinely alive and used by Granite's
// TerrainPatchRenderer::DrawBatch (via GetSharedGroundSamplers/
// ZoneGroundLayersTexture): loading + owning the shared ground textures
// (Kenshi colour overlay, per-biome ground DDS array, offline-baked
// flat-ground colour) and the per-zone ground-layer lookup texture, so
// Granite doesn't need a second ~1GB+ copy of the same data. Kept the
// class name as-is (renaming would touch every call site across game/
// tools/editor -- out of scope for this dead-code removal pass).
//
// NOT addressed here (separate, larger, riskier question — flagged, not
// investigated this pass): whether TerrainChunk's own vbo/ibo/skirt
// mesh buffers (built by terrain_gen.cpp/terrain_upload.cpp) are ALSO
// now dead now that nothing draws them directly, or whether physics/
// collision/the editor's terrain-sculpt panel still depend on them.
class TerrainRenderer {
public:
    struct SunParams {
        float dir[3];      // world-space normalised direction
        float strength;    // diffuse multiplier
        float ambient[3];  // ambient colour
        float _pad;
        // 32 bytes total — pushed as fragment uniform slot 0

        static SunParams Default() {
            SunParams s;
            s.dir[0] = 0.57f; s.dir[1] = 0.57f; s.dir[2] = 0.57f;
            s.strength   = 1.2f;
            s.ambient[0] = 0.18f; s.ambient[1] = 0.20f; s.ambient[2] = 0.26f;
            s._pad       = 0.f;
            return s;
        }
    };

    bool Init();
    void Shutdown();

    // Load Kenshi stitched colour overlay (md_terrain.png, 4096×4096).
    bool InitKenshiOverlay(const char* path);

    // Load 24-layer BC3 DDS texture array for per-biome ground texturing.
    // Also loads the paired 24-layer BC1 normal-map array (kGroundNmlPaths)
    // — currently unused by terrain_forward.slang (no normal-mapping since
    // the 2026-07-19 rewrite) but kept loaded; see biome_def.h's
    // kGroundNmlPaths comment. Must be called after Init().
    bool InitGroundTextureArray();

    // Load the offline-baked flat-ground colour (md_ground_baked.dds,
    // tools/md_bake_ground_layers.py + tools/md_encode_ground_bake.py) --
    // pre-resolved base/slope/grass/dirt/road blend (matches BlendGroundLayers'
    // sequence exactly, minus the final cliff mix) for TerrainPatchRenderer's
    // single-sample flat-ground path. Cliff stays live/triplanar (task #306,
    // see /home/rdga1/.claude/plans/serene-pondering-teapot.md).
    bool InitGroundBaked(const char* path);

    // Load the bake/live cliff_w single-source-of-truth companion
    // (md_ground_steepness_smoothed.png, tools/md_bake_ground_layers.py --
    // same per-biome-radius smoothed steepness used to classify
    // InitGroundBaked's slope_w, single channel R=steepness). Live cliff_w
    // samples THIS instead of deriving steepness from the raw per-pixel
    // normal, so the two agree by construction — see terrain_shading_
    // common.glsl's TS_ComputeGroundAlbedo for the consuming formula.
    bool InitSteepnessSmoothed(const char* path);

    // Load the biome-crossfade texture (md_biome_blend.png,
    // private/md_gen_biome_blendmap.py). STALE COMMENT FIXED 2026-09-26:
    // this used to describe an EARLIER generator version (R/G/B =
    // neighbouring-zone's base/slope/cliff GroundTexLayer index, A = blend
    // weight) -- as of the script's v6 (2026-07-19, see InitBiomeBlend's
    // own doc comment in terrain_renderer.cpp) it is a direct 1:1 copy of
    // the REAL Kenshi data/newland/land/blendmap.png instead: R/G/B/A are
    // 4 INDEPENDENT strictly-binary (0/255) masks, matching real Kenshi's
    // own blendMap mechanism (terrainfp4.hlsl) exactly -- confirmed
    // 2026-09-26 by an unrelated, independent empirical cross-check
    // (KBI1 blendinfo.dat slot-to-channel correlation, 100% purity across
    // 353 real-map samples; see re/re_docs/kenshi/terrain.md Subsystem 3).
    // Same zone-lookup-path-only scope as InitGroundBaked above — the
    // per-chunk near/mid path no longer needs this (per-vertex ground
    // selection resolves zone/chunk-boundary blending directly).
    bool InitBiomeBlend(const char* path);

    // Load the Kenshi grass/dirt/road paint mask (md_overlay_mask.png,
    // tools/md_stitch_overlay_mask.py — R=grass, G=grass2, B=dirt, A=road,
    // same UV space as tex_colour/InitKenshiOverlay). Was only consumed
    // offline (tools/md_bake_ground_layers.py) until task
    // terrain-detail-erases-road (2026-08-01): TerrainPatchRenderer's
    // close-range detail-restore layer needs this live too, so its fine
    // detail sample can favour grass/dirt/road over base ground the same
    // way the far-range bake already does — without it, close-range detail
    // silently overwrote baked-in roads with pure base texture.
    bool InitOverlayMask(const char* path);

    // КРОК 3 (docs/DAGOR_IMPLEMENTATION_PROMPT.md, docs/KROK3_DECOMPOSE_
    // SPIKE_RESULT.md, GO-verdict spike): loads tools/md_bake_detail_
    // array.py's output -- 124-layer packed BC3 array (R=brightness*0.5,
    // G=normal.x, B=normal.y; per-layer flat colour tint kept OUT of this
    // texture) + a 128x1 tint-lookup texture (nearest, index=layer/128).
    // ADDITIONAL to tex_ground_array_/tex_ground_nml_array_, not a
    // replacement -- consumed ONLY by TS_SampleZoneFlatDetail (base/slope/
    // grass/dirt/road); the cliff-triplanar path keeps sampling the
    // original full-chromatic arrays unchanged (see the bake script's own
    // "SCOPE NOTE" doc comment for why). Must be called after Init().
    bool InitDetailArray(const char* dir);

    // task-terrain-kenshi-parity (2026-09-26): loads tools/md_bake_kbi1_
    // lookup.py's output -- a 32x32 RGBA8 texture decoded from Kenshi's
    // real blendinfo.dat (KBI1 format, see re/re_docs/kenshi/terrain.md
    // Subsystem 3). Each texel's R/G/B/A byte is the biome_id (row index
    // into biome_table.txt, matching BiomeDef::biome_id) whose weight is
    // carried by md_biome_blend.png's SAME channel at that world position
    // -- confirmed empirically, slot0..3 maps directly to R/G/B/A, 100%
    // purity across 353 real-map samples (zero exceptions). 255=inactive.
    // NEAREST-filtered (a coarse per-cell lookup, not meant to interpolate
    // across cell boundaries -- the continuous cross-fade comes entirely
    // from md_biome_blend.png's own bilinear sampling, same as real Kenshi).
    bool InitKbi1BlendLookup(const char* path);

    // Companion to ZoneGroundLayersTexture/UploadZoneGroundLayers, but
    // indexed by biome_id (0..73, BiomeRegistry's small load-order list)
    // instead of zone_idx (0..4095) -- for the Kenshi-parity blend path
    // above, which resolves a biome_id directly from InitKbi1BlendLookup's
    // texture rather than going through the zone grid. SAME 28-slot
    // packing convention as zone_layers_tex_ (see UploadZoneGroundLayers's
    // own doc comment for the full slot layout) so existing TS_ZoneLayer-
    // style shader helpers can be reused unchanged, just fed a biome_id
    // instead of a zone_idx. Populated directly from BiomeRegistry's
    // already-loaded BiomeDef array (no external buffer needed, unlike
    // UploadZoneGroundLayers which depends on the caller's per-zone
    // resolution) -- call after BiomeRegistry::Get().LoadFromFile().
    void UploadBiomeLayersTex();

    bool IsReady() const;

    // Exposes 7 of the already-loaded ground-shading textures (colour
    // overlay, per-biome ground DDS array, offline-baked flat-ground colour,
    // grass/dirt/road paint mask, per-biome ground NORMAL DDS array, КРОК 3
    // packed detail array, КРОК 3 tint-lookup, in that order) —
    // TerrainPatchRenderer/Granite's DrawBatch is the sole consumer
    // now, avoiding a second, wasteful ~1GB+ reload of the same data.
    // tex_biome_blend deliberately excluded: no caller of this needs it.
    // out[4] (normal array) added task #12 (2026-09-03, "ground-texture
    // realism") -- was loaded (InitGroundTextureArray) but never exposed to
    // any shading pass, so ground detail was always lit from the flat
    // geometry normal only, never the real per-pixel normal map. out[5]/
    // out[6] (КРОК 3 detail array + tint) added 2026-09-17.
    void GetSharedGroundSamplers(SDL_GPUTextureSamplerBinding out[7]) const;
    // Per-zone (64x64=4096) ground-layer lookup — same data
    // UploadZoneGroundLayers populates, exposed for the same reuse reason.
    // A texture (R32G32B32A32_UINT, 448x64 -- 7 texels per zone, 4 uint32
    // channels each = 28 slots/zone, all used), NOT a storage buffer as of
    // 2026-08-09 (user directive to shrink Filament-blocking compute/SSBO
    // surface -- see docs/analysis/FILAMENT_MIGRATION_ANALYSIS.md). A plain
    // sampler is the one thing every rendering API (including Filament
    // materials) supports; a global read-only SSBO indexed per-pixel is not.
    md::GpuTextureHandle ZoneGroundLayersTexture() const { return zone_layers_tex_; }
    SDL_GPUSampler* ZoneGroundLayersSampler() const { return zone_layers_sampler_; }

    md::GpuTextureHandle SteepnessSmoothedTexture() const { return tex_steepness_smoothed_.SDLTexture(); }
    SDL_GPUSampler* SteepnessSmoothedSampler() const { return tex_steepness_smoothed_.SDLSampler(); }

    md::GpuTextureHandle Kbi1BlendLookupTexture() const { return kbi1_lookup_tex_.SDLTexture(); }
    SDL_GPUSampler* Kbi1BlendLookupSampler() const { return kbi1_lookup_tex_.SDLSampler(); }
    md::GpuTextureHandle BiomeLayersTexture() const { return biome_layers_tex_; }
    SDL_GPUSampler* BiomeLayersSampler() const { return biome_layers_sampler_; }
    md::GpuTextureHandle BiomeBlendTexture() const { return tex_biome_blend_.SDLTexture(); }
    SDL_GPUSampler* BiomeBlendSampler() const { return tex_biome_blend_.SDLSampler(); }

    // Upload the per-zone (64x64=4096) ground-layer lookup table: 27 of 28
    // uint32 per zone used -- [0..5] base,slope,cliff,grass,dirt,road
    // GroundTexLayer indices, [6..7] real per-biome cliff UV tiling scale
    // (bit-cast float, BiomeDef::cliff_tiling_x/y), [8] brightness_fix
    // (bit-cast float), [9..16] task #12 real per-layer UV tiling for
    // base/grass/dirt/road (bit-cast float pairs, BiomeDef::tile_base_x
    // etc), [17..18] "wavy cliff lines" distortion amplitude/wavelength
    // (bit-cast float), [19..20] task #13 real per-biome slope-layer UV
    // tiling (bit-cast float, BiomeDef::tile_slope_x/y), [21..23] task #13
    // slope-layer blend band (bit-cast float, BiomeDef::slope_min/max/
    // fade), [24..26] task #13 follow-up CLIFF blend band (bit-cast float,
    // BiomeDef::cliff_min/max/fade), [27] task БОРГ-VISUAL-3 (2026-09-06)
    // BiomeDef::biome_id, plain int (NOT bit-cast float) -- flat layout
    // index = zone_idx*28 + slot, zone_idx = zy*64+zx (matches the
    // EDITOR_TNKN=64 bitmask convention used elsewhere). Built once by the
    // caller (World3D editor's synthesis-mesh init, and SceneRender's
    // game-side equivalent) via TerrainGen_ResolveBiome() per zone.
    // count_uints must be exactly 64*64*28 = 114688. Repacks the flat
    // zone_idx*28+slot layout into the texture's 7-texels/zone layout
    // internally -- callers keep building the same flat array.
    void UploadZoneGroundLayers(const uint32_t* data, int count_uints);

private:
    GpuTexture  tex_colour_;        // Kenshi colour overlay
    GpuTexture  tex_ground_array_;  // 24-layer BC3 DDS array — the actual per-vertex-indexed ground textures
    GpuTexture  tex_ground_nml_array_; // 24-layer BC1 DDS array — paired normal maps, currently unused (no normal-mapping)
    bool        tex_loaded_        = false;
    bool        ground_array_ready_= false;

    GpuTexture  tex_ground_baked_;      // offline-baked flat-ground colour (task #306) — TerrainPatchRenderer's flat-ground sample
    bool        ground_baked_ready_ = false;
    GpuTexture  tex_steepness_smoothed_; // bake/live cliff_w single-source-of-truth (see InitSteepnessSmoothed)
    bool        steepness_smoothed_ready_ = false;
    GpuTexture  tex_biome_blend_;       // real Kenshi blendMap 1:1 copy, R/G/B/A = 4 independent binary blend-weight masks (see InitBiomeBlend's doc comment) -- consumed by the Kenshi-parity path (task-terrain-kenshi-parity, 2026-09-26)
    bool        biome_blend_ready_ = false;
    GpuTexture  tex_overlay_mask_;      // R=grass, G=grass2, B=dirt, A=road (see InitOverlayMask)
    bool        overlay_mask_ready_ = false;

    // КРОК 3 (see InitDetailArray's doc comment) -- 124-layer packed
    // brightness+normal BC3 array + 128x1 tint lookup, flat-ground-role
    // only (TS_SampleZoneFlatDetail).
    GpuTexture  tex_detail_array_;
    GpuTexture  tex_detail_tint_;
    bool        detail_array_ready_ = false;

    // Per-zone ground-layer lookup texture (see UploadZoneGroundLayers).
    md::GpuTextureHandle zone_layers_tex_     = nullptr;
    SDL_GPUSampler* zone_layers_sampler_ = nullptr;

    // task-terrain-kenshi-parity (2026-09-26): see InitKbi1BlendLookup/
    // UploadBiomeLayersTex's own doc comments above.
    GpuTexture kbi1_lookup_tex_;
    md::GpuTextureHandle biome_layers_tex_     = nullptr;
    SDL_GPUSampler* biome_layers_sampler_ = nullptr;

#ifdef MD_SDL_GPU
    md::GpuTextureHandle fallback_tex_            = nullptr;
    SDL_GPUSampler* fallback_sampler_        = nullptr;
    md::GpuTextureHandle fallback_mask_tex_       = nullptr;
    SDL_GPUSampler* fallback_mask_sampler_   = nullptr;
    md::GpuTextureHandle fallback_blend_tex_      = nullptr;
    SDL_GPUSampler* fallback_blend_sampler_  = nullptr;
    void FillSamplerBindings(SDL_GPUTextureSamplerBinding out[8]) const;
#endif
};

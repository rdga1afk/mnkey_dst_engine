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

    // Load the procedural biome-crossfade texture (md_biome_blend.png,
    // tools/md_gen_biome_blendmap.py) — R/G/B = neighbouring-zone's
    // base/slope/cliff GroundTexLayer index (0..23, packed as raw uint8/255),
    // A = blend weight (0=pure current-zone biome, 1=pure neighbour biome).
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

    bool IsReady() const;

    // Exposes 5 of the already-loaded ground-shading textures (colour
    // overlay, per-biome ground DDS array, offline-baked flat-ground colour,
    // grass/dirt/road paint mask, per-biome ground NORMAL DDS array, in that
    // order) — TerrainPatchRenderer/Granite's DrawBatch is the sole consumer
    // now, avoiding a second, wasteful ~1GB+ reload of the same data.
    // tex_biome_blend deliberately excluded: no caller of this needs it.
    // out[4] (normal array) added task #12 (2026-09-03, "ground-texture
    // realism") -- was loaded (InitGroundTextureArray) but never exposed to
    // any shading pass, so ground detail was always lit from the flat
    // geometry normal only, never the real per-pixel normal map.
    void GetSharedGroundSamplers(SDL_GPUTextureSamplerBinding out[5]) const;
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

    // task #141 (docs/research/TERRAIN_ZONE_CORNER_BAKE_PLAN.md,
    // 2026-09-16): load-time zone-corner cliff bake -- see terrain_
    // shading_common.glsl's TS_HAS_CORNER_BAKE branch (terrain_cliff_
    // blend.glsl) for the runtime lookup, terrain_zone_corner_bake.comp
    // for the bake itself. Created with a SAFE fallback state in Init()
    // (LUT filled entirely with -1 = "no flagged corner", tiny placeholder
    // atlases never actually sampled while the LUT says so) so the
    // shading pipeline's sampler count is always correct even before
    // UploadCornerBakeAtlas below has run -- a missing/wrong-sized
    // binding here is a silent-garbage class of bug on this hardware
    // (CLAUDE.md Hardware Checklist), not something to leave unbound.
    md::GpuTextureHandle CornerBakeColorAtlasTexture()  const { return corner_bake_color_tex_; }
    md::GpuTextureHandle CornerBakeNormalAtlasTexture() const { return corner_bake_normal_tex_; }
    md::GpuTextureHandle CornerBakeLutTexture()         const { return corner_bake_lut_tex_; }
    SDL_GPUSampler* CornerBakeColorAtlasSampler()  const { return corner_bake_color_sampler_; }
    SDL_GPUSampler* CornerBakeNormalAtlasSampler() const { return corner_bake_normal_sampler_; }
    SDL_GPUSampler* CornerBakeLutSampler()         const { return corner_bake_lut_sampler_; }
    int CornerBakeTilesPerRow() const { return corner_bake_tiles_per_row_; }

    // Recreates the color/normal atlas textures at real size for
    // `corner_count` flagged corners (tilesPerRow = ceil(sqrt(corner_
    // count)), tile_res fixed at 128 -- see docs/research/TERRAIN_ZONE_
    // CORNER_BAKE_PLAN.md). Caller (SceneRender::Init) runs the actual
    // compute bake afterward, writing into these via GpuComputePass's
    // rw_textures. Returns false (leaves existing placeholder textures
    // untouched) if corner_count<=0 or texture creation fails.
    bool RebuildCornerBakeAtlas(int corner_count);
    // Uploads the 65x65 grid-vertex -> atlas-tile-index LUT (-1 = not
    // flagged) -- same GpuCopyPass upload pattern as UploadZoneGroundLayers.
    void UploadCornerBakeLut(const int32_t* data65x65);

private:
    GpuTexture  tex_colour_;        // Kenshi colour overlay
    GpuTexture  tex_ground_array_;  // 24-layer BC3 DDS array — the actual per-vertex-indexed ground textures
    GpuTexture  tex_ground_nml_array_; // 24-layer BC1 DDS array — paired normal maps, currently unused (no normal-mapping)
    bool        tex_loaded_        = false;
    bool        ground_array_ready_= false;

    GpuTexture  tex_ground_baked_;      // offline-baked flat-ground colour (task #306) — TerrainPatchRenderer's flat-ground sample
    bool        ground_baked_ready_ = false;
    GpuTexture  tex_biome_blend_;       // R/G/B=neighbour base/slope/cliff idx, A=blend weight — loaded, currently unconsumed (see InitBiomeBlend)
    bool        biome_blend_ready_ = false;
    GpuTexture  tex_overlay_mask_;      // R=grass, G=grass2, B=dirt, A=road (see InitOverlayMask)
    bool        overlay_mask_ready_ = false;

    // Per-zone ground-layer lookup texture (see UploadZoneGroundLayers).
    md::GpuTextureHandle zone_layers_tex_     = nullptr;
    SDL_GPUSampler* zone_layers_sampler_ = nullptr;

    // task #141: zone-corner cliff bake atlas + LUT (see accessors above).
    md::GpuTextureHandle corner_bake_color_tex_     = nullptr;
    md::GpuTextureHandle corner_bake_normal_tex_    = nullptr;
    md::GpuTextureHandle corner_bake_lut_tex_       = nullptr;
    SDL_GPUSampler* corner_bake_color_sampler_  = nullptr;
    SDL_GPUSampler* corner_bake_normal_sampler_ = nullptr;
    SDL_GPUSampler* corner_bake_lut_sampler_    = nullptr;
    int corner_bake_tiles_per_row_ = 1;

#ifdef MD_SDL_GPU
    md::GpuTextureHandle fallback_tex_            = nullptr;
    SDL_GPUSampler* fallback_sampler_        = nullptr;
    md::GpuTextureHandle fallback_mask_tex_       = nullptr;
    SDL_GPUSampler* fallback_mask_sampler_   = nullptr;
    md::GpuTextureHandle fallback_blend_tex_      = nullptr;
    SDL_GPUSampler* fallback_blend_sampler_  = nullptr;
    void FillSamplerBindings(SDL_GPUTextureSamplerBinding out[6]) const;
#endif
};

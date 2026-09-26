#pragma once
#include <cstdint>

// Generic, data-driven biome registry. Contains NO game/IP-specific content —
// real per-biome data (ground texture paths, colours, per-zone parameters)
// is supplied at runtime via BiomeRegistry::Get().LoadFromFile(), pointing
// at a private data file outside this repo. See engine/src/world/biome_registry.cpp
// for the (also generic) file-format parser and lookup implementation.

struct BiomeDef {
    int  tex_base = 0, tex_slope = 0, tex_cliff = 0;   // indices into BiomeRegistry's ground texture table
    int  tex_grass = 0, tex_dirt = 0, tex_road = 0;    // per-biome ground layer indices
    float fog_r = 0.f, fog_g = 0.f, fog_b = 0.f;
    float sky_horizon_r = 0.f, sky_horizon_g = 0.f, sky_horizon_b = 0.f;
    // Real per-biome cliff UV tiling scale (Kenshi FCS "tiling X/Y 2",
    // confirmed against tmp_/kenshi_re/materials/deferred/terrainfp4.hlsl's
    // computeBiome() -- texCoords.yz/xz * scales0.zw). Multiplies the
    // shared world/5000 base coordinate (terrain.hlsl's main_vs); defaults
    // to 1.0 for any biome_table.txt predating this field (see
    // BiomeRegistry::LoadFromFile).
    float cliff_tiling_x = 1.0f, cliff_tiling_y = 1.0f;
    // Real per-biome albedo multiplier (Kenshi FCS "brightness fix",
    // confirmed against terrainfp4.hlsl:212 -- biome.albedo.rgb *=
    // brightnessFix.x). Defaults to 1.0 (no-op) for any biome_table.txt
    // predating this field.
    float brightness_fix = 1.0f;
    // Real per-layer UV tiling scale (Kenshi FCS "tiling X/Y", confirmed
    // against terrainfp4.hlsl's computeBiome() -- texCoords.xy * scales1.xy
    // for base, scales1.zw for grass, scales2.xy for dirt, scales2.zw for
    // road). biome_table.txt already carried these columns (tile_base_x/y
    // etc, task-terrain-brightness header) but BiomeRegistry::LoadFromFile
    // only ever kept cliff_tiling_x/y out of the 12-field tiling block --
    // task #12 (2026-09-03, "ground-texture realism") wires up the other 4
    // pairs so each layer tiles at its OWN real scale instead of one shared
    // DETAIL_TILING=90 constant for everything. Defaults to 1.0 (no-op) for
    // any biome_table.txt predating this field.
    float tile_base_x = 1.0f, tile_base_y = 1.0f;
    float tile_grass_x = 1.0f, tile_grass_y = 1.0f;
    float tile_dirt_x = 1.0f, tile_dirt_y = 1.0f;
    float tile_road_x = 1.0f, tile_road_y = 1.0f;
    // Real "wavy cliff lines" vertical-UV distortion (Kenshi FCS "distort
    // amplitude"/"distort wavelength", confirmed 2026-09-04 via live Ghidra
    // decompile of kenshi_x64.exe's ZoneMap::getTerrainMaterial_DX11, and
    // against tmp_/kenshi_re/materials/deferred/terrain.hlsl's main_vs).
    // freq = wavelength>0 ? 1/wavelength : 0; amp = distort_amplitude*0.01;
    // offset = (cos(worldX*freq)+cos(worldZ*freq))*amp, added to the cliff
    // layer's vertical mapping coordinate. Defaults to 0.0 (no-op) for any
    // biome_table.txt predating this field.
    float distort_amplitude = 0.0f, distort_wavelength = 0.0f;
    // Real per-biome slope-layer UV tiling + blend band (Kenshi FCS
    // "tiling X/Y" for the slope texture + "slope min/max/fade" weights.x
    // band, confirmed against terrainfp4.hlsl's computeBiome(): weights.x
    // = smoothstep(slopeMin-slopeBlend, slopeMin, slope) *
    //   smoothstep(slopeMax+slopeBlend, slopeMax, slope), slope = 1-N.y,
    // then `albedo = lerp(albedo, cSlope, weights.x)` inserted between the
    // grass and dirt lerps. slope_max=0.0 default (not 1.0) is a
    // deliberate disabled-sentinel: real biome rows always have
    // slope_max>0, so TS_ComputeGroundAlbedo can skip the slope layer
    // entirely for any biome_table.txt predating this field instead of
    // computing smoothstep() with degenerate matching edges.
    float tile_slope_x = 1.0f, tile_slope_y = 1.0f;
    float slope_min = 0.0f, slope_max = 0.0f, slope_fade = 0.0f;
    // Real per-biome CLIFF blend band (biome_table.txt's slope_min2/max2/
    // fade2 = terrainfp4.hlsl's weights.y band, same smoothstep-product
    // shape as slope_min/max/fade above but gating the cliff layer
    // instead). task #13 follow-up (2026-09-06): ported per owner's
    // explicit choice to use the real dual-sided band as-is, accepting a
    // known risk -- TS_ComputeGroundAlbedo's OWN commit history already
    // diagnosed and fixed a vertical-stripe flicker from this exact
    // shape (steepness=1-N.y mathematically tops out at 1.0, and 69/74
    // real biome rows have cliff_max==1.0, so ordinary per-pixel N.y
    // noise sits right on the upper edge). cliff_max=0.0 default (not
    // 1.0) is the same disabled-sentinel pattern as slope_max -- an
    // older biome_table.txt (or any zeroed/default BiomeDef) falls back
    // to TS_ComputeGroundAlbedo's original single-sided TS_CLIFF_MIN/
    // TS_CLIFF_BLEND constant instead of a degenerate smoothstep.
    float cliff_min = 0.0f, cliff_max = 0.0f, cliff_fade = 0.0f;
    // Row index into biome_table.txt's own biome list (BiomeRegistry's
    // load-order, set once in LoadFromFile — NOT re-derived from slug/legend
    // at lookup time). Purely an identity tag: lets per-zone consumers
    // (terrain_shading_common.glsl's cross-zone blend) cheaply test "do
    // these two zones share the same biome" via one int compare instead of
    // comparing every individual field (tiling/slope-band/texture indices).
    // -1 default (no BiomeDef assigned) never equals a real zone's id, so a
    // stray zeroed BiomeDef never falsely matches biome 0.
    int biome_id = -1;
};

class BiomeRegistry {
public:
    static BiomeRegistry& Get() { static BiomeRegistry inst; return inst; }

    static constexpr int MAX_BIOMES      = 128;
    static constexpr int MAX_TEXTURES    = 256;
    static constexpr int MAX_SLUG_LEN    = 48;
    static constexpr int MAX_PATH_LEN    = 160;

    // Parses the simple text format documented at the top of
    // biome_registry.cpp. Returns false (logs a warning, leaves any
    // previously-loaded state intact) if the file is missing/malformed.
    bool LoadFromFile(const char* path);

    bool Loaded() const { return biome_count_ > 0; }

    // Exact slug match; falls back to biome index 0 (or a zeroed BiomeDef
    // if nothing was ever loaded) if not found.
    const BiomeDef& ForZone(const char* zone_slug) const;

    // Nearest-colour match against each biome's registered legend RGB —
    // the real Kenshi biomemap.png lookup mechanism. Falls back the same way.
    const BiomeDef& ForColor(uint8_t r, uint8_t g, uint8_t b) const;

    int GroundTexCount() const { return tex_count_; }
    const char* GroundTexPath(int idx) const;
    const char* GroundNmlPath(int idx) const;

    // task-terrain-kenshi-parity (2026-09-26): enumeration by load-order
    // index, matching BiomeDef::biome_id (the same row index ForZone/
    // ForColor's matched entry already carries) -- for building a
    // biome_id-indexed GPU lookup texture (TerrainRenderer::
    // UploadBiomeLayersTex) from every loaded biome, not just one queried
    // by slug/colour. idx out of [0,BiomeCount()) returns the same
    // zeroed-fallback BiomeDef ForZone/ForColor use.
    int BiomeCount() const { return biome_count_; }
    const BiomeDef& ForIndex(int idx) const;

private:
    BiomeRegistry() = default;

    struct BiomeEntry {
        char slug[MAX_SLUG_LEN];
        BiomeDef def;
        uint8_t legend_rgb[3];
    };

    BiomeEntry biomes_[MAX_BIOMES] = {};
    int        biome_count_ = 0;
    BiomeDef   default_{};

    char tex_paths_[MAX_TEXTURES][MAX_PATH_LEN] = {};
    char nml_paths_[MAX_TEXTURES][MAX_PATH_LEN] = {};
    int  tex_count_ = 0;
};

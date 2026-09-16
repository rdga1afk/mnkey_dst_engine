#include <monkey_dust/render/terrain_renderer.h>
#include <monkey_dust/world/biome_def.h>
#include <monkey_dust/tools/graphics_settings.h>
#include <monkey_dust/render/render_quality.h>
#include <monkey_dust/platform/md_fs.h>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <vector>

#ifdef MD_SDL_GPU
#include <SDL3/SDL_gpu.h>
#include <monkey_dust/render/gpu_device.h>
#include <monkey_dust/render/gpu_copy_pass.h>
#include <monkey_dust/render/gpu_sampler_texture.h>
#include <monkey_dust/render/gpu_hal_free_functions.h>
#endif

bool TerrainRenderer::Init() {
#ifdef MD_SDL_GPU
    // Create 1×1 white fallback texture for slots where InitTextures was not
    // called or an individual texture failed to load.
    GpuSamplerDesc sd;
    sd.min_filter = GpuSamplerDesc::Filter::LINEAR_MIPMAP;
    sd.mag_filter = GpuSamplerDesc::Filter::LINEAR;
    sd.wrap_s     = GpuSamplerDesc::Wrap::REPEAT;
    sd.wrap_t     = GpuSamplerDesc::Wrap::REPEAT;
    sd.gen_mipmap = false;
    sd.flip_v     = false;
    uint8_t white[4] = { 255, 255, 255, 255 };
    GpuTexture fb;
    if (fb.InitFromMemory(white, 1, 1, sd)) {
        fallback_tex_     = fb.TakeSDLTexture();
        fallback_sampler_ = fb.TakeSDLSampler();
    }
    // Overlay-mask fallback: all-zero RGBA — no grass/dirt/road painted
    // anywhere, so base/slope/cliff blend alone if the mask fails to load
    // (safe default, not a wrong-looking one). Only used by the editor's
    // zone-lookup draw path (InitOverlayMask) — was created in InitPOM
    // before the 2026-07-19 rewrite, moved here since InitPOM is gone.
    uint8_t mask_neutral[4] = { 0, 0, 0, 0 };
    GpuTexture fbm;
    if (fbm.InitFromMemory(mask_neutral, 1, 1, sd)) {
        fallback_mask_tex_     = fbm.TakeSDLTexture();
        fallback_mask_sampler_ = fbm.TakeSDLSampler();
    }
    // Biome-blend fallback: A=0 -- no cross-fade, pure current-zone biome.
    // Same zone-lookup-path-only scope as the mask fallback above.
    uint8_t blend_neutral[4] = { 0, 0, 0, 0 };
    GpuTexture fbb;
    if (fbb.InitFromMemory(blend_neutral, 1, 1, sd)) {
        fallback_blend_tex_     = fbb.TakeSDLTexture();
        fallback_blend_sampler_ = fbb.TakeSDLSampler();
    }

    // Per-zone (64x64=4096) ground-layer lookup texture -- see
    // UploadZoneGroundLayers. 27 uint32 used of 28 per zone: [0..5]
    // base,slope,cliff,grass,dirt,road GroundTexLayer indices; [6..7] real
    // per-biome cliff UV tiling scale (FCS "tiling X/Y 2", confirmed
    // against terrainfp4.hlsl), bit-cast float (uintBitsToFloat in the
    // shader); [8] real per-biome brightness_fix (FCS "brightness fix",
    // terrainfp4.hlsl:212), also bit-cast float; [9..16] task #12
    // (2026-09-03) real per-layer UV tiling for base/grass/dirt/road
    // (bit-cast float pairs, matches BiomeDef::tile_base_x etc) -- was
    // previously one shared DETAIL_TILING=90 constant for every layer in
    // every biome, ignoring biome_table.txt's own real per-layer values;
    // [17..18] real per-biome "wavy cliff lines" distortion amplitude/
    // wavelength (2026-09-04, Ghidra-verified against kenshi_x64.exe's
    // ZoneMap::getTerrainMaterial_DX11 and terrain.hlsl's main_vs), also
    // bit-cast float; [19..20] task #13 (2026-09-05) real per-biome
    // slope-layer UV tiling (BiomeDef::tile_slope_x/y), bit-cast float;
    // [21..23] real per-biome slope-layer blend band (BiomeDef::
    // slope_min/max/fade, terrainfp4.hlsl's weights.x), bit-cast float;
    // [24..26] task #13 follow-up (2026-09-06) real per-biome CLIFF blend
    // band (BiomeDef::cliff_min/max/fade, terrainfp4.hlsl's weights.y),
    // bit-cast float; [27] BiomeDef::biome_id (task БОРГ-VISUAL-3,
    // 2026-09-06) -- plain int, NOT bit-cast float, the zone's biome
    // identity for the shader's cross-zone blend to compare cheaply
    // ("do these two corner zones share a biome") instead of comparing
    // every individual field. R32G32B32A32_UINT, 448x64 (7
    // texels/zone x 4 channels = 28 slots, all used) -- a texture, not an
    // SSBO, as of 2026-08-09 (see ZoneGroundLayersTexture's header doc
    // comment for why). Allocated empty here; populated once by the
    // caller (World3D editor's synthesis-mesh init, and SceneRender's
    // game-side equivalent).
    {
        md::GpuDeviceHandle dev = md::GpuDevice::Get().SDLDevice();
        SDL_GPUTextureCreateInfo ti{};
        ti.type                 = SDL_GPU_TEXTURETYPE_2D;
        ti.format               = SDL_GPU_TEXTUREFORMAT_R32G32B32A32_UINT;
        ti.width                = 64 * 7;
        ti.height                = 64;
        ti.layer_count_or_depth = 1;
        ti.num_levels           = 1;
        ti.usage                = SDL_GPU_TEXTUREUSAGE_SAMPLER;
        zone_layers_tex_ = GpuCreateTexture(dev, &ti);

        SDL_GPUSamplerCreateInfo si{};
        si.min_filter     = SDL_GPU_FILTER_NEAREST;
        si.mag_filter     = SDL_GPU_FILTER_NEAREST;
        si.mipmap_mode    = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
        si.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
        si.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
        si.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
        zone_layers_sampler_ = GpuCreateSampler(dev, &si);
    }

    // task #141: zone-corner cliff bake atlas + LUT -- see terrain_
    // renderer.h's accessor doc comment. LUT filled with -1 (int32) here,
    // synchronously, so the shading pipeline is safe to draw with BEFORE
    // the real bake (SceneRender's load sequence, once implemented) ever
    // runs -- the TS_HAS_CORNER_BAKE runtime branch reads this LUT and
    // takes the live per-pixel path whenever it sees -1, identical output
    // to before this feature existed.
    {
        md::GpuDeviceHandle dev = md::GpuDevice::Get().SDLDevice();
        SDL_GPUTextureCreateInfo lut_ti{};
        lut_ti.type                 = SDL_GPU_TEXTURETYPE_2D;
        lut_ti.format               = SDL_GPU_TEXTUREFORMAT_R32_INT;
        lut_ti.width                = 65;
        lut_ti.height               = 65;
        lut_ti.layer_count_or_depth = 1;
        lut_ti.num_levels           = 1;
        lut_ti.usage                = SDL_GPU_TEXTUREUSAGE_SAMPLER;
        corner_bake_lut_tex_ = GpuCreateTexture(dev, &lut_ti);

        SDL_GPUSamplerCreateInfo lut_si{};
        lut_si.min_filter     = SDL_GPU_FILTER_NEAREST;
        lut_si.mag_filter     = SDL_GPU_FILTER_NEAREST;
        lut_si.mipmap_mode    = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
        lut_si.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
        lut_si.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
        lut_si.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
        corner_bake_lut_sampler_ = GpuCreateSampler(dev, &lut_si);

        // 1x1 placeholder atlases -- never actually sampled while the LUT
        // above reads -1 everywhere, but must be real, correctly-typed
        // bound textures (SDL_GPU has no "unbound sampler" concept for a
        // declared shader binding). Real bake (terrain_zone_corner_bake.
        // comp) replaces both via a compute-storage-write usage texture,
        // recreated at real size once the flagged-corner count is known.
        SDL_GPUTextureCreateInfo atlas_ti{};
        atlas_ti.type                 = SDL_GPU_TEXTURETYPE_2D;
        atlas_ti.format               = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
        atlas_ti.width                = 1;
        atlas_ti.height               = 1;
        atlas_ti.layer_count_or_depth = 1;
        atlas_ti.num_levels           = 1;
        atlas_ti.usage                = SDL_GPU_TEXTUREUSAGE_SAMPLER
                                       | SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_WRITE;
        corner_bake_color_tex_  = GpuCreateTexture(dev, &atlas_ti);
        corner_bake_normal_tex_ = GpuCreateTexture(dev, &atlas_ti);

        SDL_GPUSamplerCreateInfo atlas_si{};
        atlas_si.min_filter     = SDL_GPU_FILTER_LINEAR;
        atlas_si.mag_filter     = SDL_GPU_FILTER_LINEAR;
        atlas_si.mipmap_mode    = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
        atlas_si.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
        atlas_si.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
        atlas_si.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
        corner_bake_color_sampler_  = GpuCreateSampler(dev, &atlas_si);
        corner_bake_normal_sampler_ = GpuCreateSampler(dev, &atlas_si);

        if (corner_bake_lut_tex_) {
            std::vector<int32_t> lut_init((size_t)65 * 65, -1);
            SDL_GPUTransferBufferCreateInfo tbi{};
            tbi.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
            tbi.size  = (Uint32)(lut_init.size() * sizeof(int32_t));
            SDL_GPUTransferBuffer* tb = GpuCreateTransferBuffer(dev, &tbi);
            if (tb) {
                void* map = GpuMapTransfer(tb, false);
                if (map) memcpy(map, lut_init.data(), tbi.size);
                GpuUnmapTransfer(tb);

                md::GpuCommandBufferHandle cmd = md::GpuDevice::Get().AcquireCommandBuffer();
                GpuCopyPass cp;
                cp.Begin(cmd);
                SDL_GPUTextureTransferInfo src{};
                src.transfer_buffer = tb;
                src.pixels_per_row  = 65;
                src.rows_per_layer  = 65;
                SDL_GPUTextureRegion dst{};
                dst.texture = corner_bake_lut_tex_;
                dst.w = 65; dst.h = 65; dst.d = 1;
                cp.UploadTexture(src, dst, false);
                cp.End();
                md::GpuDevice::Get().Submit(cmd);
                GpuReleaseTransferBuffer(dev, tb);
            }
        }
    }
#endif
    return true;
}

bool TerrainRenderer::InitGroundTextureArray()
{
#ifdef MD_SDL_GPU
    // Note: InitFromDDSArray ignores this sampler desc entirely and always
    // creates its own LINEAR_MIPMAP_LINEAR sampler from the DDS's own mip
    // chain (see gpu_hal_buffers.cpp) -- kept here only as the function's
    // required argument shape.
    GpuSamplerDesc sd;
    sd.min_filter = GpuSamplerDesc::Filter::LINEAR_MIPMAP;
    sd.mag_filter = GpuSamplerDesc::Filter::LINEAR;
    sd.wrap_s     = GpuSamplerDesc::Wrap::REPEAT;
    sd.wrap_t     = GpuSamplerDesc::Wrap::REPEAT;
    sd.gen_mipmap = false;
    const BiomeRegistry& biomes = BiomeRegistry::Get();
    const int tex_count = biomes.GroundTexCount();
    const char* dif_ptrs[BiomeRegistry::MAX_TEXTURES];
    const char* nml_ptrs[BiomeRegistry::MAX_TEXTURES];
    for (int i = 0; i < tex_count; ++i) {
        dif_ptrs[i] = biomes.GroundTexPath(i);
        nml_ptrs[i] = biomes.GroundNmlPath(i);
    }

    tex_ground_array_.Shutdown();
    if (!tex_ground_array_.InitFromDDSArray(dif_ptrs, tex_count, sd)) {
        fprintf(stderr, "[TerrainRenderer] ground texture array failed\n");
        ground_array_ready_ = false;
        return false;
    }
    tex_ground_nml_array_.Shutdown();
    if (!tex_ground_nml_array_.InitFromDDSArray(nml_ptrs, tex_count, sd)) {
        fprintf(stderr, "[TerrainRenderer] ground normal array failed — terrain lighting falls back to flat vertex normal\n");
        ground_array_ready_ = false;
        return false;
    }
    ground_array_ready_ = true;
    fprintf(stderr, "[TerrainRenderer] ground texture+normal arrays ready (%d layers)\n", tex_count);
    return true;
#else
    return false;
#endif
}

bool TerrainRenderer::InitGroundBaked(const char* path)
{
#ifdef MD_SDL_GPU
    // Offline-baked flat-ground colour (task #306, tools/md_bake_ground_
    // layers.py + tools/md_encode_ground_bake.py) -- replaces the old
    // live per-pixel base/slope/grass/dirt/road blend (GetSharedGroundSamplers'
    // 3rd slot used to be tex_overlay_mask_, the painted grass/dirt/road
    // mask itself; that live blend chain moved offline, so this slot now
    // holds the RESULT of that blend instead of one of its inputs).
    GpuSamplerDesc sd;
    sd.min_filter = GpuSamplerDesc::Filter::LINEAR_MIPMAP;
    sd.mag_filter = GpuSamplerDesc::Filter::LINEAR;
    sd.wrap_s     = GpuSamplerDesc::Wrap::CLAMP_TO_EDGE;
    sd.wrap_t     = GpuSamplerDesc::Wrap::CLAMP_TO_EDGE;
    sd.gen_mipmap = true;
    sd.flip_v     = false;

    tex_ground_baked_.Shutdown();
    if (!tex_ground_baked_.InitFromFile(path, sd)) {
        fprintf(stderr, "[TerrainRenderer] ground baked texture failed: %s\n", path);
        ground_baked_ready_ = false;
        return false;
    }
    ground_baked_ready_ = true;
    fprintf(stdout, "[TerrainRenderer] ground baked texture loaded: %s\n", path);
    return true;
#else
    return false;
#endif
}

bool TerrainRenderer::InitBiomeBlend(const char* path)
{
#ifdef MD_SDL_GPU
    GpuSamplerDesc sd;
    // LINEAR safe: this file (private/md_gen_biome_blendmap.py, v6,
    // 2026-07-19) is now a direct 1:1 copy of the REAL Kenshi
    // data/newland/land/blendmap.png -- confirmed every channel is
    // strictly binary 0/255 (spot-checked at copy time), and R/G/B/A are 4
    // INDEPENDENT masks (not identical, 27%/25%/17%/24% nonzero) -- the
    // smooth ramp comes entirely from the GPU's own bilinear sampling of
    // this binary mask, not pre-blurred data, matching real Kenshi's own
    // mechanism (terrainfp4.hlsl).
    sd.min_filter = GpuSamplerDesc::Filter::LINEAR;
    sd.mag_filter = GpuSamplerDesc::Filter::LINEAR;
    sd.wrap_s     = GpuSamplerDesc::Wrap::CLAMP_TO_EDGE;
    sd.wrap_t     = GpuSamplerDesc::Wrap::CLAMP_TO_EDGE;
    sd.gen_mipmap = false;
    sd.flip_v     = false;

    tex_biome_blend_.Shutdown();
    if (!tex_biome_blend_.InitFromFile(path, sd)) {
        fprintf(stderr, "[TerrainRenderer] biome blend map failed: %s\n", path);
        biome_blend_ready_ = false;
        return false;
    }
    biome_blend_ready_ = true;
    fprintf(stdout, "[TerrainRenderer] biome blend map loaded: %s\n", path);
    return true;
#else
    return false;
#endif
}

bool TerrainRenderer::InitOverlayMask(const char* path)
{
#ifdef MD_SDL_GPU
    GpuSamplerDesc sd;
    // Same rationale as InitBiomeBlend: LINEAR (not LINEAR_MIPMAP) — this is
    // a mask sampled at a live per-fragment UV close to the camera, not a
    // pre-blurred colour texture, and generating mips of a thin-line road
    // mask would fade it out at exactly the mid distances the detail-restore
    // layer covers.
    sd.min_filter = GpuSamplerDesc::Filter::LINEAR;
    sd.mag_filter = GpuSamplerDesc::Filter::LINEAR;
    sd.wrap_s     = GpuSamplerDesc::Wrap::CLAMP_TO_EDGE;
    sd.wrap_t     = GpuSamplerDesc::Wrap::CLAMP_TO_EDGE;
    sd.gen_mipmap = false;
    sd.flip_v     = false;

    tex_overlay_mask_.Shutdown();

    // Fast path: the same pre-baked .raw sidecar clutter_gen.cpp's
    // s_load_grass_density() already uses (tools/md_stitch_overlay_mask.py)
    // -- a plain fread instead of stb_image's ~760ms+ single-threaded PNG
    // DEFLATE decode of this 4096x4096 image (measured here: InitFromFile's
    // PNG path alone added ~1.9s to game startup). Header = 2x little-endian
    // uint32 (width,height) then raw RGBA8 bytes; falls back to the .png if
    // the sidecar is missing/stale (it's gitignored, regenerate-only).
    uint32_t raw_len = 0;
    char* raw = md::fs_read_alloc("game/data/textures/md_overlay_mask.raw", &raw_len);
    if (raw && raw_len > 8) {
        uint32_t w, h;
        memcpy(&w, raw,     4);
        memcpy(&h, raw + 4, 4);
        bool ok = (uint64_t)raw_len - 8 == (uint64_t)w * h * 4
                  && tex_overlay_mask_.InitFromMemory(reinterpret_cast<const uint8_t*>(raw + 8),
                                                       (int)w, (int)h, sd);
        md::fs_free(raw);
        if (ok) {
            overlay_mask_ready_ = true;
            fprintf(stdout, "[TerrainRenderer] overlay mask loaded (raw sidecar): %ux%u\n", w, h);
            return true;
        }
    }

    if (!tex_overlay_mask_.InitFromFile(path, sd)) {
        fprintf(stderr, "[TerrainRenderer] overlay mask failed: %s\n", path);
        overlay_mask_ready_ = false;
        return false;
    }
    overlay_mask_ready_ = true;
    fprintf(stdout, "[TerrainRenderer] overlay mask loaded: %s\n", path);
    return true;
#else
    return false;
#endif
}

void TerrainRenderer::Shutdown() {
    tex_colour_.Shutdown();
    tex_ground_array_.Shutdown();
    tex_ground_nml_array_.Shutdown();
    tex_ground_baked_.Shutdown();
    ground_baked_ready_ = false;
    tex_biome_blend_.Shutdown();
    biome_blend_ready_ = false;
    tex_overlay_mask_.Shutdown();
    overlay_mask_ready_ = false;
    tex_loaded_         = false;
    ground_array_ready_ = false;

#ifdef MD_SDL_GPU
    md::GpuDeviceHandle dev = md::GpuDevice::Get().SDLDevice();
    if (dev) {
        if (fallback_sampler_)       GpuReleaseSampler(dev, fallback_sampler_);
        if (fallback_tex_)           GpuReleaseTexture(dev, fallback_tex_);
        if (fallback_mask_sampler_)  GpuReleaseSampler(dev, fallback_mask_sampler_);
        if (fallback_mask_tex_)      GpuReleaseTexture(dev, fallback_mask_tex_);
        if (fallback_blend_sampler_) GpuReleaseSampler(dev, fallback_blend_sampler_);
        if (fallback_blend_tex_)     GpuReleaseTexture(dev, fallback_blend_tex_);
        if (zone_layers_sampler_)    GpuReleaseSampler(dev, zone_layers_sampler_);
        if (zone_layers_tex_)        GpuReleaseTexture(dev, zone_layers_tex_);
        if (corner_bake_color_sampler_)  GpuReleaseSampler(dev, corner_bake_color_sampler_);
        if (corner_bake_color_tex_)      GpuReleaseTexture(dev, corner_bake_color_tex_);
        if (corner_bake_normal_sampler_) GpuReleaseSampler(dev, corner_bake_normal_sampler_);
        if (corner_bake_normal_tex_)     GpuReleaseTexture(dev, corner_bake_normal_tex_);
        if (corner_bake_lut_sampler_)    GpuReleaseSampler(dev, corner_bake_lut_sampler_);
        if (corner_bake_lut_tex_)        GpuReleaseTexture(dev, corner_bake_lut_tex_);
    }
    zone_layers_tex_     = nullptr;
    zone_layers_sampler_ = nullptr;
    corner_bake_color_tex_      = nullptr;
    corner_bake_color_sampler_  = nullptr;
    corner_bake_normal_tex_     = nullptr;
    corner_bake_normal_sampler_ = nullptr;
    corner_bake_lut_tex_        = nullptr;
    corner_bake_lut_sampler_    = nullptr;
    fallback_tex_            = nullptr;
    fallback_sampler_        = nullptr;
    fallback_mask_tex_       = nullptr;
    fallback_mask_sampler_   = nullptr;
    fallback_blend_tex_      = nullptr;
    fallback_blend_sampler_  = nullptr;
#endif
}

bool TerrainRenderer::IsReady() const {
    // No own draw pipeline anymore (see class doc comment) -- readiness now
    // just means the shared ground textures Granite depends on are loaded.
    return ground_array_ready_ && ground_baked_ready_;
}

bool TerrainRenderer::InitKenshiOverlay(const char* path)
{
    GpuSamplerDesc sd;
    sd.min_filter = GpuSamplerDesc::Filter::LINEAR_MIPMAP;
    sd.mag_filter = GpuSamplerDesc::Filter::LINEAR;
    sd.wrap_s     = GpuSamplerDesc::Wrap::CLAMP_TO_EDGE;
    sd.wrap_t     = GpuSamplerDesc::Wrap::CLAMP_TO_EDGE;
    sd.gen_mipmap = true;
    sd.flip_v     = false;

    tex_colour_.Shutdown();
    if (!tex_colour_.InitFromFile(path, sd)) {
        fprintf(stderr, "[TerrainRenderer] kenshi overlay failed: %s\n", path);
        return false;
    }
    tex_loaded_ = true;
    fprintf(stdout, "[TerrainRenderer] kenshi overlay loaded: %s\n", path);
    return true;
}


#ifdef MD_SDL_GPU
void TerrainRenderer::FillSamplerBindings(SDL_GPUTextureSamplerBinding out[6]) const
{
    bool valid = tex_loaded_ && tex_colour_.Valid()
                 && tex_colour_.SDLTexture() && tex_colour_.SDLSampler();
    out[0].texture = valid ? tex_colour_.SDLTexture() : fallback_tex_;
    out[0].sampler = valid ? tex_colour_.SDLSampler() : fallback_sampler_;
    // b1: per-biome DDS ground array — the actual per-vertex-indexed ground
    // textures (see terrain_gen.cpp's s_ground_pick / TerrainVertex).
    bool ga = ground_array_ready_ && tex_ground_array_.Valid()
              && tex_ground_array_.SDLTexture() && tex_ground_array_.SDLSampler();
    out[1].texture = ga ? tex_ground_array_.SDLTexture() : nullptr;
    out[1].sampler = ga ? tex_ground_array_.SDLSampler() : nullptr;
    // b2: offline-baked flat-ground colour (task #306) — TerrainPatchRenderer's
    // flat-ground path only (was the painted grass/dirt/road mask before
    // that blend moved offline; the per-chunk near/mid path never used
    // this slot, resolving ground layers at generation time instead).
    bool mv = ground_baked_ready_ && tex_ground_baked_.Valid()
              && tex_ground_baked_.SDLTexture() && tex_ground_baked_.SDLSampler();
    out[2].texture = mv ? tex_ground_baked_.SDLTexture() : fallback_mask_tex_;
    out[2].sampler = mv ? tex_ground_baked_.SDLSampler() : fallback_mask_sampler_;
    // b3: procedural biome-crossfade blend map — loaded, currently unconsumed
    // (GetSharedGroundSamplers only exposes 3 of these 4 slots to Granite).
    bool bv = biome_blend_ready_ && tex_biome_blend_.Valid()
              && tex_biome_blend_.SDLTexture() && tex_biome_blend_.SDLSampler();
    out[3].texture = bv ? tex_biome_blend_.SDLTexture() : fallback_blend_tex_;
    out[3].sampler = bv ? tex_biome_blend_.SDLSampler() : fallback_blend_sampler_;
    // b4: grass/dirt/road paint mask (task terrain-detail-erases-road,
    // 2026-08-01) — TerrainPatchRenderer's close-range detail-restore layer
    // only; reuses fallback_mask_tex_/sampler_ (all-zero — no grass/dirt/
    // road painted anywhere, safe default) since it's the same neutral
    // shape this slot already had before InitPOM's removal.
    bool om = overlay_mask_ready_ && tex_overlay_mask_.Valid()
              && tex_overlay_mask_.SDLTexture() && tex_overlay_mask_.SDLSampler();
    out[4].texture = om ? tex_overlay_mask_.SDLTexture() : fallback_mask_tex_;
    out[4].sampler = om ? tex_overlay_mask_.SDLSampler() : fallback_mask_sampler_;
    // b5: per-biome ground NORMAL DDS array, paired 1:1 with tex_ground_
    // array's diffuse layers (InitGroundTextureArray loads both from the
    // same biome_table.txt index list). task #12 (2026-09-03) -- loaded
    // since the array's introduction but never exposed here before, so no
    // shading pass ever sampled it.
    bool na = ground_array_ready_ && tex_ground_nml_array_.Valid()
              && tex_ground_nml_array_.SDLTexture() && tex_ground_nml_array_.SDLSampler();
    out[5].texture = na ? tex_ground_nml_array_.SDLTexture() : nullptr;
    out[5].sampler = na ? tex_ground_nml_array_.SDLSampler() : nullptr;
}

void TerrainRenderer::GetSharedGroundSamplers(SDL_GPUTextureSamplerBinding out[5]) const {
    SDL_GPUTextureSamplerBinding all[6];
    FillSamplerBindings(all);
    out[0] = all[0];  // tex_colour
    out[1] = all[1];  // tex_ground_array
    out[2] = all[2];  // tex_ground_baked
    out[3] = all[4];  // tex_overlay_mask
    out[4] = all[5];  // tex_ground_nml_array
}
#endif

void TerrainRenderer::UploadZoneGroundLayers(const uint32_t* data, int count_uints) {
#ifdef MD_SDL_GPU
    if (count_uints != 64 * 64 * 28) {
        fprintf(stderr, "[TerrainRenderer] UploadZoneGroundLayers: expected %d uints, got %d — skipped\n",
                64 * 64 * 28, count_uints);
        return;
    }
    if (!zone_layers_tex_) return;
    md::GpuDeviceHandle dev = md::GpuDevice::Get().SDLDevice();
    if (!dev) return;

    // Repack the caller's flat zone_idx*28+slot layout into the texture's
    // 7-texels-per-zone x 4-channels layout (28 slots, 27 used -- see
    // ZoneGroundLayersTexture's header doc comment).
    const int W = 64 * 7, H = 64;
    std::vector<uint32_t> packed((size_t)W * H * 4, 0u);
    for (int zy = 0; zy < 64; ++zy) {
        for (int zx = 0; zx < 64; ++zx) {
            int zone_idx = zy * 64 + zx;
            for (int slot = 0; slot < 28; ++slot) {
                int t = slot / 4, c = slot % 4;
                size_t texel_idx = (size_t)zy * W + (size_t)(zx * 7 + t);
                packed[texel_idx * 4 + (size_t)c] = data[(size_t)zone_idx * 28 + (size_t)slot];
            }
        }
    }

    SDL_GPUTransferBufferCreateInfo tbi{};
    tbi.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    tbi.size  = (Uint32)(packed.size() * sizeof(uint32_t));
    SDL_GPUTransferBuffer* tb = GpuCreateTransferBuffer(dev, &tbi);
    if (!tb) return;
    void* map = GpuMapTransfer(tb, false);
    if (map) memcpy(map, packed.data(), tbi.size);
    GpuUnmapTransfer(tb);

    md::GpuCommandBufferHandle cmd = md::GpuDevice::Get().AcquireCommandBuffer();
    GpuCopyPass cp;
    cp.Begin(cmd);
    SDL_GPUTextureTransferInfo src{};
    src.transfer_buffer = tb;
    src.pixels_per_row  = (Uint32)W;
    src.rows_per_layer  = (Uint32)H;
    SDL_GPUTextureRegion dst{};
    dst.texture = zone_layers_tex_;
    dst.w = (Uint32)W; dst.h = (Uint32)H; dst.d = 1;
    cp.UploadTexture(src, dst, false);
    cp.End();
    md::GpuDevice::Get().Submit(cmd);
    GpuReleaseTransferBuffer(dev, tb);
#else
    (void)data; (void)count_uints;
#endif
}

bool TerrainRenderer::RebuildCornerBakeAtlas(int corner_count) {
#ifdef MD_SDL_GPU
    if (corner_count <= 0) return false;
    md::GpuDeviceHandle dev = md::GpuDevice::Get().SDLDevice();
    if (!dev) return false;

    int tiles_per_row = (int)std::ceil(std::sqrt((double)corner_count));
    if (tiles_per_row < 1) tiles_per_row = 1;
    const int TILE_RES = 128;
    const int atlas_dim = tiles_per_row * TILE_RES;

    SDL_GPUTextureCreateInfo ti{};
    ti.type                 = SDL_GPU_TEXTURETYPE_2D;
    ti.format                = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    ti.width                 = (Uint32)atlas_dim;
    ti.height                = (Uint32)atlas_dim;
    ti.layer_count_or_depth = 1;
    ti.num_levels            = 1;
    ti.usage                 = SDL_GPU_TEXTUREUSAGE_SAMPLER
                              | SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_WRITE;
    md::GpuTextureHandle new_color  = GpuCreateTexture(dev, &ti);
    md::GpuTextureHandle new_normal = GpuCreateTexture(dev, &ti);
    if (!new_color || !new_normal) {
        if (new_color)  GpuReleaseTexture(dev, new_color);
        if (new_normal) GpuReleaseTexture(dev, new_normal);
        fprintf(stderr, "[TerrainRenderer] RebuildCornerBakeAtlas: texture create failed (%dx%d)\n",
                atlas_dim, atlas_dim);
        return false;
    }

    if (corner_bake_color_tex_)  GpuReleaseTexture(dev, corner_bake_color_tex_);
    if (corner_bake_normal_tex_) GpuReleaseTexture(dev, corner_bake_normal_tex_);
    corner_bake_color_tex_     = new_color;
    corner_bake_normal_tex_    = new_normal;
    corner_bake_tiles_per_row_ = tiles_per_row;
    fprintf(stderr, "[TerrainRenderer] RebuildCornerBakeAtlas: %d corners, %dx%d tiles, atlas %dx%d\n",
            corner_count, tiles_per_row, tiles_per_row, atlas_dim, atlas_dim);
    return true;
#else
    (void)corner_count;
    return false;
#endif
}

void TerrainRenderer::UploadCornerBakeLut(const int32_t* data65x65) {
#ifdef MD_SDL_GPU
    if (!corner_bake_lut_tex_) return;
    md::GpuDeviceHandle dev = md::GpuDevice::Get().SDLDevice();
    if (!dev) return;

    SDL_GPUTransferBufferCreateInfo tbi{};
    tbi.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    tbi.size  = (Uint32)(65 * 65 * sizeof(int32_t));
    SDL_GPUTransferBuffer* tb = GpuCreateTransferBuffer(dev, &tbi);
    if (!tb) return;
    void* map = GpuMapTransfer(tb, false);
    if (map) memcpy(map, data65x65, tbi.size);
    GpuUnmapTransfer(tb);

    md::GpuCommandBufferHandle cmd = md::GpuDevice::Get().AcquireCommandBuffer();
    GpuCopyPass cp;
    cp.Begin(cmd);
    SDL_GPUTextureTransferInfo src{};
    src.transfer_buffer = tb;
    src.pixels_per_row  = 65;
    src.rows_per_layer  = 65;
    SDL_GPUTextureRegion dst{};
    dst.texture = corner_bake_lut_tex_;
    dst.w = 65; dst.h = 65; dst.d = 1;
    cp.UploadTexture(src, dst, false);
    cp.End();
    md::GpuDevice::Get().Submit(cmd);
    GpuReleaseTransferBuffer(dev, tb);
#else
    (void)data65x65;
#endif
}

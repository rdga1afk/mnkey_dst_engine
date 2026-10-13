#ifdef MD_SDL_GPU
#include <monkey_dust/render/terrain_shading_projected.h>
#include <monkey_dust/render/gpu_device.h>
#include <monkey_dust/render/gpu_hal_free_functions.h>
#include <monkey_dust/render/ambient_probe.h>
#include <monkey_dust/platform/md_log.h>
#include <cstring>
#include <cmath>

// Divergence #7 prototype (md.set_terrain_ground_aniso, game/src/scripting/lua_scenario_api.cpp; read by npc_render_frame_prep.cpp for the
// shader flag): 0 = off (the DDS arrays' own LINEAR samplers, textureLod in the shader); N >= 1 = textureGrad in the shader and a LINEAR sampler
// with max_anisotropy = N for tex_ground / tex_ground_nml (N = 1: gradients without anisotropy, to separate the two costs).
int g_terrain_ground_aniso_level = 0;
// Divergence #14 (md.set_terrain_ambientmap): Kenshi's regional ambientmap.png multiplies the sun (2*alpha) and the ambient light (rgb) in
// TS_ApplyLighting (deferred.hlsl:197,213-214). Default true (owner decision 2026-10-09, shader sample verified against the PNG). Carried to the shader as
// +1600 on |pix_scale| (bit4), only when the texture loaded.
bool g_terrain_ambientmap = true; // owner decision 2026-10-09: ON (verified against the PNG, scripts/verify_ambientmap.py)

namespace {
// One sampler per level, created lazily and kept for the process lifetime (a handful of 64-byte objects; the device outlives them).
SDL_GPUSampler* GroundAnisoSampler(int level) {
    static SDL_GPUSampler* cache[17] = {};
    if (level < 1 || level > 16) return nullptr;
    if (!cache[level]) {
        SDL_GPUSamplerCreateInfo si = {};
        si.min_filter        = SDL_GPU_FILTER_LINEAR;
        si.mag_filter        = SDL_GPU_FILTER_LINEAR;
        si.mipmap_mode       = SDL_GPU_SAMPLERMIPMAPMODE_LINEAR;
        si.address_mode_u    = SDL_GPU_SAMPLERADDRESSMODE_REPEAT;
        si.address_mode_v    = SDL_GPU_SAMPLERADDRESSMODE_REPEAT;
        si.address_mode_w    = SDL_GPU_SAMPLERADDRESSMODE_REPEAT;
        si.min_lod           = 0.f;
        si.max_lod           = 15.f; // Vulkan clamps to the image's real mip count (same upper bound the DDS array loader uses: ref_mips-1)
        si.enable_anisotropy = level > 1;
        si.max_anisotropy    = (float)level;
        cache[level] = SDL_CreateGPUSampler(md::GpuDevice::Get().SDLDevice(), &si);
    }
    return cache[level];
}
}


// Mirrors terrain_patch_renderer.cpp's PatchFragUBO / terrain_baked_renderer.
// cpp's BakedPatchFragUBO -- each pipeline file keeps its own copy of these
// small POD UBO structs rather than sharing one header (established
// convention in this codebase, see terrain_baked_renderer.cpp).
struct ProjFragUBO {
    float sun_dir_str[4];
    float ambient[4];
    float world_params[4];
    float fog_color_near[4];
    float fog_far;
    float _pad[3];
};
static_assert(sizeof(ProjFragUBO) == 80, "ProjFragUBO size mismatch");

struct ProjCamUBO {
    float cam_pos_ws[4];
    float inv_vp[16];  // S7: inverse view-projection (column-major, same layout as the vertex VP UBO)
    float recon[4];    // S7: x=world_extent, y=heightmap resolution (texels), z=mode (0 off,1 reconstruct)
};
static_assert(sizeof(ProjCamUBO) == 96, "ProjCamUBO size mismatch");

bool TerrainShadingProjected::CreateTextures(int w, int h) {
    w_ = w; h_ = h;

    GpuSamplerDesc gs;
    gs.min_filter = GpuSamplerDesc::Filter::NEAREST;
    gs.mag_filter = GpuSamplerDesc::Filter::NEAREST;
    gs.wrap_s = GpuSamplerDesc::Wrap::CLAMP_TO_EDGE;
    gs.wrap_t = GpuSamplerDesc::Wrap::CLAMP_TO_EDGE;
    if (!gbuf_color_.InitRenderTarget(w, h, gs, SDL_GPU_TEXTUREFORMAT_R16G16B16A16_UINT)) {
        MD_LOG(MD_LOG_WARNING, "[TerrainShadingProjected] gbuf_color_ create failed");
        return false;
    }
    gbuf_depth_.Init(w, h, /*shadow_border=*/false);
    if (!gbuf_depth_.SDLTexture()) {
        MD_LOG(MD_LOG_WARNING, "[TerrainShadingProjected] gbuf_depth_ create failed");
        return false;
    }
    return true;
}

bool TerrainShadingProjected::Init(md::GpuDeviceHandle dev, int w, int h) {
    if (!CreateTextures(w, h)) return false;

    GpuPipeline::Desc rd;
    rd.vert_path = "shaders/terrain_shading_screenspace.vert";
    rd.frag_path = "shaders/terrain_shading_screenspace.frag";
    rd.layout.count  = 0;
    rd.layout.stride = 0;
    rd.raster.depth_test      = true;
    rd.raster.depth_write     = false;
    rd.raster.cull_back       = false;
    rd.raster.depth_compare_op = SDL_GPU_COMPAREOP_LESS_OR_EQUAL;
    rd.has_depth_target   = true;
    rd.vert_uniform_bufs  = 0;
    rd.vert_samplers      = 0;
    rd.frag_uniform_bufs  = 2;  // set=3 binding=0 ProjFragUBO, binding=1 ProjCamUBO
    rd.frag_samplers      = 16; // set=2: tex_colour,tex_ground,tex_ground_baked,tex_overlay_mask,tex_ground_nml (task #12), tex_detail_array,tex_detail_tint (КРОК3 2026-09-17); gbufPacked,gbufDepth; zoneGroundLayersTex (texture, not SSBO, since 2026-08-09); texSteepnessSmoothed (2026-09-24, bake/live cliff_w single-source-of-truth); tex_biome_blend/kbi1LookupTex/biomeLayersTex (task-terrain-kenshi-parity, 2026-09-26). БОРГ-TERRAIN-2 (2026-09-13): was 10 -- vtIndirection/vtAtlas removed, dead VT cache-hit path never called. 2026-09-26: was 11 -- task #141's zone-corner cliff bake atlas (3 samplers) removed then task-terrain-kenshi-parity's 3 samplers added back, see CLAUDE_HISTORY.md.
    // 2026-09-19 (docs/RESOLVE_OPT.md session finding): AmbientProbeBuf,
    // binding=15 (was 14 until the ambientmap sampler took 14, 2026-10-09; after the 15 samplers 0-14 above) --
    // see terrain_shading_common.glsl's TS_HAS_AMBIENT_PROBE doc comment.
    // Was 0 since БОРГ-TERRAIN-2 (2026-09-13, vtPageMeta removed with TerrainVtPageCache).
    rd.frag_storage_bufs  = 1;
    if (!resolve_pipeline_.Create(rd)) {
        MD_LOG(MD_LOG_WARNING, "[TerrainShadingProjected] resolve pipeline create failed");
        return false;
    }
    // task #227: Kenshi-parity variant (debug-only) -- failure here is non-fatal, the toggle then
    // falls back to the default pipeline's base-layer-only Kenshi branch.
    rd.frag_path = "shaders/terrain_shading_screenspace_kenshi.frag";
    resolve_kenshi_ready_ = resolve_kenshi_pipeline_.Create(rd);
    if (!resolve_kenshi_ready_) {
        MD_LOG(MD_LOG_WARNING, "[TerrainShadingProjected] kenshi resolve variant create failed (debug toggle falls back)");
    }
    // #155 step 1: the same variant without the flat-chain skip on cliff_w >= 1 (A/B, SetSkipFlatOnCliff(false)); non-fatal
    rd.frag_path = "shaders/terrain_shading_screenspace_kenshi_noskip.frag";
    resolve_kenshi_noskip_ready_ = resolve_kenshi_noskip_pipeline_.Create(rd);
    // #155 step 2b: the Kenshi resolve with the ground cache read; the default variant above has no ring code
    rd.frag_path = "shaders/terrain_shading_screenspace_kenshi_ring.frag";
    resolve_kenshi_ring_ready_ = resolve_kenshi_ring_pipeline_.Create(rd);
    // #155 step 2: ground cache bake variant: same inputs, one RGBA16UI colour target, no depth
    {
        GpuPipeline::Desc bd = rd;
        bd.frag_path = "shaders/terrain_ground_bake.frag";
        bd.raster.depth_test = false;
        bd.has_depth_target = false;
        bd.color_format = SDL_GPU_TEXTUREFORMAT_R16G16B16A16_UINT;
        bake_ready_ = bake_pipeline_.Create(bd);
        if (!bake_ready_) MD_LOG(MD_LOG_WARNING, "[TerrainShadingProjected] ground cache bake pipeline create failed (the cache stays off)");
    }

    ready_ = true;
    MD_LOG(MD_LOG_INFO, "[TerrainShadingProjected] ready %dx%d (RGBA32F gbuffer + isolated D32_FLOAT)", w, h);
    return true;
}

// Only the two render-target textures depend on window size -- resolve_pipeline_
// depends solely on texture FORMAT (unchanged across a resize), so it must never
// be touched here. The original version of this function called the full Init()
// (destroying+recreating the pipeline too), which meant every resize destroyed a
// GPU pipeline object that a still-in-flight command buffer from the previous
// frame could still reference -- a real SIGSEGV inside the Intel Vulkan driver,
// diagnosed live 2026-08-02 even after relocating the call to before
// AcquireCommandBuffer (that relocation alone only fixed the FIRST-frame case).
void TerrainShadingProjected::EnsureSize(md::GpuDeviceHandle dev, int w, int h) {
    (void)dev;
    if (!ready_ || (w == w_ && h == h_) || w <= 0 || h <= 0) return;
    gbuf_depth_.Shutdown();
    gbuf_color_.Shutdown();
    CreateTextures(w, h);
}

void TerrainShadingProjected::Shutdown() {
    resolve_pipeline_.Destroy();
    resolve_kenshi_pipeline_.Destroy();
    resolve_kenshi_ready_ = false;
    resolve_kenshi_noskip_pipeline_.Destroy();
    resolve_kenshi_noskip_ready_ = false;
    resolve_kenshi_ring_pipeline_.Destroy();
    resolve_kenshi_ring_ready_ = false;
    bake_pipeline_.Destroy();
    bake_ready_ = false;
    ring_color_.Shutdown();
    ring_ready_ = false;
    ring_valid_ = false;
    gbuf_depth_.Shutdown();
    gbuf_color_.Shutdown();
    ready_ = false;
}

SDL_GPURenderPass* TerrainShadingProjected::BeginGBufferPass(md::GpuCommandBufferHandle cmd) {
    if (!ready_) return nullptr;

    // clear_color MUST be set explicitly to {0,0,0,0} -- ColorPassDesc's own
    // default is {0,0,0,1} (alpha=1), which would silently corrupt this
    // G-buffer's alpha channel (same class of bug caught in smaa_system.cpp;
    // see docs/HAL_CLOSURE_PROGRESS.md's "documented lesson" on this default).
    GpuCommandBuffer cb;
    GpuCommandBuffer::ColorPassDesc cpd;
    cpd.cmd            = cmd;
    cpd.color_tex[0]      = gbuf_color_.SDLTexture();
    cpd.depth_tex      = gbuf_depth_.SDLTexture();
    cpd.clear_color[0] = 0.f; cpd.clear_color[1] = 0.f;
    cpd.clear_color[2] = 0.f; cpd.clear_color[3] = 0.f;
    cpd.clear_depth    = 1.f;
    cpd.load_color     = false; // CLEAR
    cpd.load_depth     = false; // CLEAR
    cb.BeginColorPass(cpd);
    return cb.SDLPass();
}

void TerrainShadingProjected::EndGBufferPass() {
    // Caller keeps the SDL_GPURenderPass* returned by BeginGBufferPass to
    // draw with -- ending it is a plain SDL call, no state kept here (same
    // pattern as GBuffer::End, which also doesn't need internal pass_
    // tracking since the caller already holds the pointer). Intentionally
    // a no-op body: callers call SDL_EndGPURenderPass(rp) themselves, this
    // exists only so BeginGBufferPass/EndGBufferPass read as a matched pair
    // at call sites (mirrors GBuffer::Begin/End's shape without duplicating
    // its internal pass_ member for a pass this class doesn't otherwise
    // need to remember between calls).
}

void TerrainShadingProjected::DrawFullscreen(SDL_GPURenderPass* rp, md::GpuCommandBufferHandle cmd, GpuPipeline& pipeline, const ResolveCtx& c, bool bake,
                                              const FineWindow* fw, const float* bake_cols) {
    GpuPassView pv = GpuPassView::FromRaw(rp, cmd);
    pv.BindPipeline(&pipeline);
    const TerrainRenderer::SunParams& sun = *c.sun;
    const TerrainRenderer& ground = *c.ground;
    const float cam_x = c.cam_x, cam_y = c.cam_y, cam_z = c.cam_z, world_origin_x = c.world_origin_x, world_origin_z = c.world_origin_z, world_to_uv = c.world_to_uv;
    const float fog_far = c.fog_far, fog_near = c.fog_near; const float* fog_color = c.fog_color;
    const bool shade_constant_debug = c.shade_constant_debug, kenshi_blend_debug = c.kenshi_blend_debug;

    ProjFragUBO fubo{};
    fubo.sun_dir_str[0] = sun.dir[0]; fubo.sun_dir_str[1] = sun.dir[1];
    fubo.sun_dir_str[2] = sun.dir[2]; fubo.sun_dir_str[3] = sun.strength;
    fubo.ambient[0]     = sun.ambient[0]; fubo.ambient[1] = sun.ambient[1];
    fubo.ambient[2]     = sun.ambient[2];
    fubo.ambient[3]     = units10_ ? 1.f : 0.f; // .w = "terrain_units10" flag (the shader's ambient uses .xyz only)
    fubo.world_params[0] = world_origin_x; fubo.world_params[1] = world_origin_z;
    fubo.world_params[2] = world_to_uv;
    // Крок 0 ablation (shade_constant_debug=1.0) / task-terrain-kenshi-
    // parity (kenshi_blend_debug=2.0, checked first in the shader) --
    // see header doc comment.
    fubo.world_params[3] = kenshi_blend_debug ? 2.f : (shade_constant_debug ? 1.f : 0.f);
    fubo.fog_color_near[0] = fog_color[0]; fubo.fog_color_near[1] = fog_color[1];
    fubo.fog_color_near[2] = fog_color[2]; fubo.fog_color_near[3] = fog_near;
    fubo.fog_far = fog_far;
    GpuPushFragmentUniforms(cmd, 0, &fubo, sizeof(fubo));

    ProjCamUBO cubo{};
    cubo.cam_pos_ws[0] = cam_x; cubo.cam_pos_ws[1] = cam_y;
    cubo.cam_pos_ws[2] = cam_z; cubo.cam_pos_ws[3] = (g_terrain_ambientmap && ground.AmbientMapReady())
        ? (pix_scale_ < 0.f ? pix_scale_ - 1600.f : pix_scale_ + 1600.f) : pix_scale_; // bit4 = ambientmap (see g_terrain_ambientmap)
    if (bake) {
        // bake: inv_vp holds the ring / fine-window columns (see terrain_shading_screenspace.frag, TS_BAKE); recon is unused there
        std::memset(cubo.inv_vp, 0, sizeof(cubo.inv_vp));
        std::memcpy(cubo.inv_vp, bake_cols, 12 * sizeof(float));
        cubo.recon[0] = (float)h_; // the screen's height in pixels: the live shader's analytic mip uses the pixel angle (pix_scale / G-buffer height), the bake must too
    } else {
        std::memcpy(cubo.inv_vp, s7_.inv_vp, sizeof(cubo.inv_vp));
        // recon = the ground cache ring for the resolve: xy = centre, z = texel size (m), w = ring size (0 = not used). The S7 values that lived here
        // (world extent / resolution / mode) were read by no shader since the 8 B G-buffer.
        if (ring_use_ && ring_ready_ && ring_valid_) { cubo.recon[0] = ring_cx_; cubo.recon[1] = ring_cz_; cubo.recon[2] = ring_texel_m_; cubo.recon[3] = (ring_count_ > 1 ? -1.f : 1.f) * (float)ring_n_; }
        else { cubo.recon[0] = cubo.recon[1] = cubo.recon[2] = cubo.recon[3] = 0.f; }
    }
    GpuPushFragmentUniforms(cmd, 1, &cubo, sizeof(cubo));

    // set=2: same 7 shared ground samplers the normal forward terrain draw
    // binds (TerrainPatchRenderer::DrawBatch) -- index 4 (tex_ground_nml)
    // added task #12 (2026-09-03); index 5/6 (КРОК 3 packed detail array +
    // tint) added 2026-09-17.
    SDL_GPUTextureSamplerBinding ground_bindings[7];
    ground.GetSharedGroundSamplers(ground_bindings);
    if (g_terrain_ground_aniso_level >= 1) { // slots 1 = tex_ground, 4 = tex_ground_nml (see the 7-binding layout above)
        if (SDL_GPUSampler* as = GroundAnisoSampler(g_terrain_ground_aniso_level)) {
            ground_bindings[1].sampler = as;
            ground_bindings[4].sampler = as;
        }
    }
    // All 7 must be checked, not just index 0 -- unlike slots 0/2/3/6
    // (plain sampler2D, always backed by a same-typed 1x1 fallback texture
    // even when their real asset fails to load), slots 1/4/5
    // (tex_ground_array, tex_ground_nml_array, tex_detail_array, all
    // sampler2DArray in the shader) have no fallback of a matching image
    // type in FillSamplerBindings and fall back to nullptr/nullptr.
    for (int i = 0; i < 7; ++i) {
        if (!ground_bindings[i].texture || !ground_bindings[i].sampler) return;
    }
    pv.BindFragmentSamplers(0, ground_bindings, 7);
    // No SSBO left in this set (БОРГ-TERRAIN-2, 2026-09-13: vtPageMeta
    // removed with the rest of TerrainVtPageCache) -- zoneGroundLayers is
    // a texture, not an SSBO, since 2026-08-09.

    // set=1: this class's own G-buffer (packed world-pos/normal + dedicated depth).
    const bool s7_recon = (s7_.mode == 1 && s7_.normal_tex && s7_.normal_samp);
    SDL_GPUTextureSamplerBinding gbuf_bindings[2] = {
        // S7 mode 1: binding 7 carries the world normal texture (gbuf_color_ is not written).
        s7_recon ? SDL_GPUTextureSamplerBinding{ s7_.normal_tex, s7_.normal_samp }
                 : SDL_GPUTextureSamplerBinding{ gbuf_color_.SDLTexture(), gbuf_color_.SDLSampler() },
        { gbuf_depth_.SDLTexture(), gbuf_depth_.SDLSampler() },
    };
    if (bake) { // the bake variant reads the fine height window at 7 (and has nothing at 8)
        gbuf_bindings[0] = { fw->tex, fw->samp };
        gbuf_bindings[1] = { fw->tex, fw->samp };
    }
    pv.BindFragmentSamplers(7, gbuf_bindings, 2);

    // Zone ground-layer lookup -- binding=9 (КРОК 3, 2026-09-17: was 7,
    // shifted +2 after inserting tex_detail_array/tex_detail_tint above),
    // texture not SSBO since 2026-08-09 (Filament-blocker reduction, see
    // ZoneGroundLayersTexture's header doc comment). Continues the same
    // contiguous sampler run.
    SDL_GPUTextureSamplerBinding zone_binding[1] = {
        { ground.ZoneGroundLayersTexture(), ground.ZoneGroundLayersSampler() },
    };
    pv.BindFragmentSamplers(9, zone_binding, 1);

    // bake/live cliff_w single-source-of-truth (2026-09-24), binding=10
    // (2026-09-26: was 13, shifted -3 after removing task #141's
    // zone-corner cliff bake atlas -- see CLAUDE_HISTORY.md) --
    // continues the same contiguous sampler run. See terrain_
    // shading_screenspace.frag's texSteepnessSmoothed doc comment.
    SDL_GPUTextureSamplerBinding steepness_binding[1] = {
        { ground.SteepnessSmoothedTexture(), ground.SteepnessSmoothedSampler() },
    };
    pv.BindFragmentSamplers(10, steepness_binding, 1);

    // task-terrain-kenshi-parity (2026-09-26): the three Kenshi-parity
    // biome-blend data sources, binding=11/12/13 -- see terrain_
    // shading_screenspace.frag's own binding doc comments for the full
    // rationale. Continues the same contiguous sampler run.
    SDL_GPUTextureSamplerBinding kenshi_bindings[3] = {
        { ground.BiomeBlendTexture(),      ground.BiomeBlendSampler() },
        { ground.Kbi1BlendLookupTexture(), ground.Kbi1BlendLookupSampler() },
        { ground.BiomeLayersTexture(),     ground.BiomeLayersSampler() },
    };
    // Unlike ground_bindings above, these three have no same-typed 1x1
    // fallback on load failure (LoadTerrainTexture/UploadBiomeLayersTex
    // leave the handle null rather than substituting a placeholder) --
    // must check before binding, same reasoning as the ground_bindings
    // loop's own comment.
    for (int i = 0; i < 3; ++i) {
        if (!kenshi_bindings[i].texture || !kenshi_bindings[i].sampler) return;
    }
    pv.BindFragmentSamplers(11, kenshi_bindings, 3);

    // Divergence #14: binding=14, Kenshi's ambientmap.png. Falls back to the (always loaded) biome-blend texture as a type-matching
    // dummy when the file is missing -- the shader only reads it when bit4 of |pix_scale| is set, which needs AmbientMapReady().
    SDL_GPUTextureSamplerBinding ambient_binding[1] = {
        ground.AmbientMapReady() ? SDL_GPUTextureSamplerBinding{ ground.AmbientMapTexture(), ground.AmbientMapSampler() }
                                 : SDL_GPUTextureSamplerBinding{ ground.BiomeBlendTexture(), ground.BiomeBlendSampler() },
    };
    pv.BindFragmentSamplers(14, ambient_binding, 1);

    // #155 step 2: binding 15 = the ground cache ring (read by the resolve when recon.w > 0); gbuf_color_ is the same format and stands in while the cache is
    // off and in the bake draw (the ring is its own render target there).
    SDL_GPUTextureSamplerBinding ring_binding[1] = {
        (!bake && ring_use_ && ring_ready_ && ring_valid_) ? SDL_GPUTextureSamplerBinding{ ring_color_.SDLTexture(), ring_color_.SDLSampler() }
                                                           : SDL_GPUTextureSamplerBinding{ gbuf_color_.SDLTexture(), gbuf_color_.SDLSampler() },
    };
    pv.BindFragmentSamplers(15, ring_binding, 1);

    // 2026-09-19 (docs/RESOLVE_OPT.md session finding): directional
    // ambient via AmbientProbeSystem, binding=14 (2026-09-26: was 11,
    // shifted +3 after adding the 3 Kenshi-parity samplers above) --
    // see terrain_shading_common.glsl's TS_HAS_AMBIENT_PROBE doc comment.
    SDL_GPUBuffer* ambient_probe_buf = AmbientProbeSystem::Get().GetSSBO().SDLBuffer();
    pv.BindFragmentStorageBuffers(0, &ambient_probe_buf, 1);

    pv.Draw(3, 1, 0, 0);
}

void TerrainShadingProjected::ConfigureRing(int n, float texel_m, int rings) {
    rings = std::max(1, std::min(2, rings));
    if (n == ring_n_ && texel_m == ring_texel_m_ && rings == ring_count_) return;
    ring_count_ = rings; ring_have_ = false; ring_building_ = false;
    ring_color_.Shutdown();
    ring_ready_ = false; ring_valid_ = false;
    ring_n_ = n; ring_texel_m_ = texel_m;
}

bool TerrainShadingProjected::EnsureGroundRing() {
    if (ring_ready_) return true;
    if (!ready_ || !bake_ready_) return false;
    GpuSamplerDesc gs;
    gs.min_filter = GpuSamplerDesc::Filter::NEAREST;
    gs.mag_filter = GpuSamplerDesc::Filter::NEAREST;
    gs.wrap_s = GpuSamplerDesc::Wrap::CLAMP_TO_EDGE;
    gs.wrap_t = GpuSamplerDesc::Wrap::CLAMP_TO_EDGE;
    if (!ring_color_.InitRenderTarget(ring_n_, ring_n_ * ring_count_, gs, SDL_GPU_TEXTUREFORMAT_R16G16B16A16_UINT)) {
        MD_LOG(MD_LOG_WARNING, "[TerrainShadingProjected] ground cache ring create failed");
        return false;
    }
    ring_ready_ = true;
    return true;
}

int TerrainShadingProjected::UpdateGroundRing(md::GpuCommandBufferHandle cmd, float cam_x, float cam_y, float cam_z, float world_origin_x, float world_origin_z,
                                               float world_to_uv, const TerrainRenderer& ground, const FineWindow& fw, float target_x, float target_z,
                                               int budget_texels, bool force_full) {
    if (!EnsureGroundRing() || !fw.tex || !fw.samp) return 0;
    const int N = ring_n_, mask = N - 1, R = ring_count_;
    const int tx = (int)std::floor(target_x / ring_texel_m_ + 0.5f), tz = (int)std::floor(target_z / ring_texel_m_ + 0.5f);
    struct Rect { int ring, x, y, w, h; };
    Rect rects[32]; int nr = 0; int texels = 0;
    auto add = [&](int k, int x, int y, int w, int h) { if (w > 0 && h > 0 && nr < 32) { rects[nr++] = { k, x, y, w, h }; texels += w * h; } };
    // a strip of `len` physical columns (col) / rows of ring k starting at world texel `start`, split where it wraps
    auto strip = [&](int k, bool col, int start, int len) {
        const int s = start & mask, l1 = std::min(len, N - s);
        if (col) { add(k, s, 0, l1, N); add(k, 0, 0, len - l1, N); } else { add(k, 0, s, N, l1); add(k, 0, 0, N, len - l1); }
    };
    int ci[2][2] = { { ring_ci_x_, ring_ci_z_ }, { ring_ci1_x_, ring_ci1_z_ } };  // centre texel of each ring (ring 1: ceil(c0 / 2), in its own texels)
    auto derive1 = [&](int c0) { return (c0 + 1) >> 1; };
    const int rows_per_call = std::max(1, budget_texels / N);
    bool restart = force_full || !ring_have_;
    if (!restart && !ring_building_ && std::max(std::abs(tx - ring_ci_x_), std::abs(tz - ring_ci_z_)) >= N * 3 / 4) restart = true;
    if (restart) {
        ring_ci_x_ = tx; ring_ci_z_ = tz; ring_ci1_x_ = derive1(tx); ring_ci1_z_ = derive1(tz);
        ring_have_ = true; ring_building_ = true; ring_fill_row_[0] = ring_fill_row_[1] = 0; ring_valid_ = false;
    } else if (!ring_building_) {
        const int kHyst = 8; // ring 0 texels: scroll in batches rather than a sliver every frame
        const int max_step = std::max(1, budget_texels / (3 * N));
        auto step = [&](int d) { return std::abs(d) >= kHyst ? std::max(-max_step, std::min(max_step, d)) : 0; };
        const int dx = step(tx - ring_ci_x_), dz = step(tz - ring_ci_z_);
        if (dx != 0 || dz != 0) {
            for (int k = 0; k < R; ++k) {
                const int ox = ci[k][0], oz = ci[k][1];
                const int nx = k == 0 ? ox + dx : derive1(ring_ci_x_ + dx), nz = k == 0 ? oz + dz : derive1(ring_ci_z_ + dz);
                const int ddx = nx - ox, ddz = nz - oz;
                // the physical columns / rows of the texels that left (moving +) or the ones that enter (moving -) are the ones to rebake
                if (ddx != 0) strip(k, true,  ddx > 0 ? ox - N / 2 : nx - N / 2, std::abs(ddx));
                if (ddz != 0) strip(k, false, ddz > 0 ? oz - N / 2 : nz - N / 2, std::abs(ddz));
                if (k == 1) { ring_ci1_x_ = nx; ring_ci1_z_ = nz; }
            }
            ring_ci_x_ += dx; ring_ci_z_ += dz;
        }
    }
    if (ring_building_) {
        int left = force_full ? N * R : rows_per_call;
        bool done = true;
        for (int k = 0; k < R; ++k) {
            const int rows = std::min(left, N - ring_fill_row_[k]);
            add(k, 0, ring_fill_row_[k], N, rows);
            ring_fill_row_[k] += rows; left -= rows;
            if (ring_fill_row_[k] < N) done = false;
        }
        if (done) { ring_building_ = false; ring_valid_ = true; }
    }
    ring_cx_ = ring_ci_x_ * ring_texel_m_; ring_cz_ = ring_ci_z_ * ring_texel_m_;
    if (nr == 0) return 0;

    GpuCommandBuffer cb;
    GpuCommandBuffer::ColorPassDesc cpd;
    cpd.cmd = cmd;
    cpd.color_tex[0] = ring_color_.SDLTexture();
    cpd.load_color = true; // the rest of the ring stays
    cb.BeginColorPass(cpd);
    SDL_GPURenderPass* rp = cb.SDLPass();
    if (!rp) return 0;
    TerrainRenderer::SunParams sun{};
    const float fog_col[3] = {0.f, 0.f, 0.f};
    ResolveCtx c;
    c.sun = &sun; c.cam_x = cam_x; c.cam_y = cam_y; c.cam_z = cam_z;
    c.world_origin_x = world_origin_x; c.world_origin_z = world_origin_z; c.world_to_uv = world_to_uv;
    c.fog_far = 1.f; c.fog_color = fog_col; c.fog_near = 0.f;
    c.ground = &ground; c.shade_constant_debug = false; c.kenshi_blend_debug = true;
    const int cur[2][2] = { { ring_ci_x_, ring_ci_z_ }, { ring_ci1_x_, ring_ci1_z_ } };
    for (int i = 0; i < nr; ++i) {
        const int k = rects[i].ring;
        float cols[12] = { (float)(cur[k][0] - N / 2), (float)(cur[k][1] - N / 2), ring_texel_m_ * (float)(1 << k), (float)N,
                           fw.origin_x_m, fw.origin_z_m, fw.hmin, fw.hrange,  (float)fw.cells, fw.cell_m, (float)(k * N), 0.f };
        SDL_Rect sc = { rects[i].x, rects[i].y + k * N, rects[i].w, rects[i].h };
        SDL_SetGPUScissor(rp, &sc);
        DrawFullscreen(rp, cmd, bake_pipeline_, c, true, &fw, cols);
    }
    SDL_EndGPURenderPass(rp);
    return texels;
}

void TerrainShadingProjected::DrawShadingResolve(SDL_GPURenderPass* rp, md::GpuCommandBufferHandle cmd,
                                                   const TerrainRenderer::SunParams& sun,
                                                   float cam_x, float cam_y, float cam_z,
                                                   float world_origin_x, float world_origin_z, float world_to_uv,
                                                   float fog_far, const float fog_color[3], float fog_near,
                                                   const TerrainRenderer& ground,
                                                   bool shade_constant_debug,
                                                   bool kenshi_blend_debug) {
    if (!ready_) return;

    // RESOLVE_OPT spatial-split plan, Крок 4/6 (docs/RESOLVE_OPT.md,
    // 2026-09-19): four fullscreen draws instead of one -- each pipeline
    // discards every category except its own (cheap=0, zone=1, cliff=2,
    // full=3). Resource bindings (UBOs, samplers) are IDENTICAL for all
    // four -- only the bound pipeline differs -- but rebound per-draw
    // rather than assumed to persist across BindPipeline, since that
    // persistence isn't verified for this HAL wrapper and the cost of
    // rebinding is negligible next to the fragment-shader win this split
    // exists for.
    ResolveCtx c;
    c.sun = &sun; c.cam_x = cam_x; c.cam_y = cam_y; c.cam_z = cam_z;
    c.world_origin_x = world_origin_x; c.world_origin_z = world_origin_z; c.world_to_uv = world_to_uv;
    c.fog_far = fog_far; c.fog_color = fog_color; c.fog_near = fog_near;
    c.ground = &ground; c.shade_constant_debug = shade_constant_debug; c.kenshi_blend_debug = kenshi_blend_debug;
    // RESOLVE_OPT 4-draw split reverted to a single draw -- see
    // resolve_pipeline_'s own doc comment (terrain_shading_projected.h)
    // for the full measured rationale.
    const bool want_ring_variant = (ring_use_ && ring_ready_ && ring_valid_) || ring_variant_forced_;
    GpuPipeline* kp = &resolve_kenshi_pipeline_;
    if (!skip_flat_on_cliff_ && resolve_kenshi_noskip_ready_) kp = &resolve_kenshi_noskip_pipeline_; // the A/B variant keeps its ring code (not used with the cache)
    else if (want_ring_variant && resolve_kenshi_ring_ready_) kp = &resolve_kenshi_ring_pipeline_;
    DrawFullscreen(rp, cmd, (kenshi_blend_debug && resolve_kenshi_ready_) ? *kp : resolve_pipeline_, c, false, nullptr, nullptr);
}
#endif

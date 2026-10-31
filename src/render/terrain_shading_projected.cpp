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
int g_ground_variant = -1;   // measurement (#207 variant_eval): the active pass-B variant of $MD_SPV_VARIANTS, set by md.set_ground_variant
int g_terrain_ground_aniso_level = 1;   // ground layers: 1 = mip from the pixel footprint (textureGrad, default), 2..16 = + anisotropy (owner decision 17.10, #180)
int g_terrain_ground_min_lod = 0;   // sampler min_lod of the ground layers in the textureGrad path: 1 = what Kenshi's "texture resolution gimping 1" gives (the largest mip is never read)
// Divergence #14 (md.set_terrain_ambientmap): Kenshi's regional ambientmap.png multiplies the sun (2*alpha) and the ambient light (rgb) in
// TS_ApplyLighting (deferred.hlsl:197,213-214). Default true (owner decision 2026-10-09, shader sample verified against the PNG). Carried to the shader as
// +1600 on |pix_scale| (bit4), only when the texture loaded.
bool g_terrain_ambientmap = true; // owner decision 2026-10-09: ON (verified against the PNG, scripts/verify_ambientmap.py)

namespace {
// One sampler per level, created lazily and kept for the process lifetime (a handful of 64-byte objects; the device outlives them).
SDL_GPUSampler* GroundAnisoSampler(int level) {
    static SDL_GPUSampler* cache[17][4] = {};
    if (level < 1 || level > 16) return nullptr;
    const int ml = g_terrain_ground_min_lod < 0 ? 0 : (g_terrain_ground_min_lod > 3 ? 3 : g_terrain_ground_min_lod);
    if (!cache[level][ml]) {
        SDL_GPUSamplerCreateInfo si = {};
        si.min_filter        = SDL_GPU_FILTER_LINEAR;
        si.mag_filter        = SDL_GPU_FILTER_LINEAR;
        si.mipmap_mode       = SDL_GPU_SAMPLERMIPMAPMODE_LINEAR;
        si.address_mode_u    = SDL_GPU_SAMPLERADDRESSMODE_REPEAT;
        si.address_mode_v    = SDL_GPU_SAMPLERADDRESSMODE_REPEAT;
        si.address_mode_w    = SDL_GPU_SAMPLERADDRESSMODE_REPEAT;
        si.min_lod           = (float)ml;
        si.max_lod           = 15.f; // Vulkan clamps to the image's real mip count (same upper bound the DDS array loader uses: ref_mips-1)
        si.enable_anisotropy = level > 1;
        si.max_anisotropy    = (float)level;
        cache[level][ml] = SDL_CreateGPUSampler(md::GpuDevice::Get().SDLDevice(), &si);
    }
    return cache[level][ml];
}
// measurement (#207 H7a): the same sampler with mipmap_mode NEAREST (bilinear inside one mip instead of trilinear), bound for the variants listed in $MD_VARIANT_NEARMIP (comma list of variant indices)
SDL_GPUSampler* GroundNearMipSampler() {
    static SDL_GPUSampler* s = nullptr;
    if (!s) {
        SDL_GPUSamplerCreateInfo si = {};
        si.min_filter = si.mag_filter = SDL_GPU_FILTER_LINEAR;
        si.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
        si.address_mode_u = si.address_mode_v = si.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_REPEAT;
        si.min_lod = (float)(g_terrain_ground_min_lod < 0 ? 0 : (g_terrain_ground_min_lod > 3 ? 3 : g_terrain_ground_min_lod));
        si.max_lod = 15.f;
        s = SDL_CreateGPUSampler(md::GpuDevice::Get().SDLDevice(), &si);
    }
    return s;
}
bool VariantEnvBit(const char* name, int v) {   // measurement (#207): a comma / digit list of variant indices in an environment variable
    const char* e = getenv(name); if (!e) return false;
    for (const char* c = e; *c; ++c) if (*c - '0' == v) return true;
    return false;
}
bool VariantNearMip(int v) {
    static unsigned mask = [] { unsigned m = 0; if (const char* e = getenv("MD_VARIANT_NEARMIP")) for (const char* c = e; *c; ++c) if (*c >= '0' && *c <= '7') m |= 1u << (*c - '0'); return m; }();
    return v >= 0 && v < 8 && (mask >> v & 1u);
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
    rd.frag_path = "shaders/terrain_shading_screenspace_kenshi.frag";
    rd.layout.count  = 0;
    rd.layout.stride = 0;
    rd.raster.depth_test      = false;
    rd.raster.depth_write     = false;
    rd.raster.cull_back       = false;
    rd.raster.depth_compare_op = SDL_GPU_COMPAREOP_LESS_OR_EQUAL;
    rd.has_depth_target   = true;
    rd.vert_uniform_bufs  = 0;
    rd.vert_samplers      = 0;
    rd.frag_uniform_bufs  = 3;  // set=3 binding=0 ProjFragUBO, binding=1 ProjCamUBO, binding=2 BiomeParams (the biome table, #167 P0)
    rd.frag_samplers      = 16; // set=2: tex_colour,tex_ground,tex_ground_baked,tex_overlay_mask,tex_ground_nml (task #12), tex_detail_array,tex_detail_tint (КРОК3 2026-09-17); gbufPacked,gbufDepth; zoneGroundLayersTex (texture, not SSBO, since 2026-08-09); texSteepnessSmoothed (2026-09-24, bake/live cliff_w single-source-of-truth); tex_biome_blend/kbi1LookupTex/biomeLayersTex (task-terrain-kenshi-parity, 2026-09-26). БОРГ-TERRAIN-2 (2026-09-13): was 10 -- vtIndirection/vtAtlas removed, dead VT cache-hit path never called. 2026-09-26: was 11 -- task #141's zone-corner cliff bake atlas (3 samplers) removed then task-terrain-kenshi-parity's 3 samplers added back, see CLAUDE_HISTORY.md.
    // 2026-09-19 (docs/RESOLVE_OPT.md session finding): AmbientProbeBuf,
    // binding=15 (was 14 until the ambientmap sampler took 14, 2026-10-09; after the 15 samplers 0-14 above) --
    // see terrain_shading_common.glsl's TS_HAS_AMBIENT_PROBE doc comment.
    // Was 0 since БОРГ-TERRAIN-2 (2026-09-13, vtPageMeta removed with TerrainVtPageCache).
    rd.frag_storage_bufs  = 2;   // binding 16 AmbientProbeBuf, binding 17 BiomeParamsBuf (TS_BIOME_SSBO variants)
    rd.frag_path = "shaders/terrain_shading_screenspace_kenshi.frag";
    resolve_kenshi_ready_ = resolve_kenshi_pipeline_.Create(rd);
    if (!resolve_kenshi_ready_) {
        MD_LOG(MD_LOG_WARNING, "[TerrainShadingProjected] kenshi resolve pipeline create failed");
        return false;
    }
    // #155 step 1: the same variant without the flat-chain skip on cliff_w >= 1 (A/B, SetSkipFlatOnCliff(false)); non-fatal
    rd.frag_path = "shaders/terrain_shading_screenspace_kenshi_noskip.frag";
    resolve_kenshi_noskip_ready_ = resolve_kenshi_noskip_pipeline_.Create(rd);
    // #163 5b-2b: the reuse variant: slot 2 (baked ground) = previous G-buffer depth, slot 15 = history colour (SDL_GPU binds at most 16 fragment samplers), a 4th UBO (the reprojection); non-fatal
    {
        GpuPipeline::Desc ud = rd;
        ud.frag_path = "shaders/terrain_shading_screenspace_kenshi_reuse.frag";
        ud.frag_uniform_bufs = 4;
        resolve_kenshi_reuse_ready_ = resolve_kenshi_reuse_pipeline_.Create(ud);
        if (!resolve_kenshi_reuse_ready_) MD_LOG(MD_LOG_WARNING, "[TerrainShadingProjected] reuse resolve variant create failed (ground reuse unavailable)");
    }
    {   // first of all pipelines, so the first variant (= ref, the same shader as the shipped pass B) is the one that gets compiled and dumped by the driver
        SDL_GPUDevice* vdev = md::GpuDevice::Get().SDLDevice();
        if (vdev && SDL_GPUTextureSupportsFormat(vdev, SDL_GPU_TEXTUREFORMAT_D32_FLOAT_S8_UINT, SDL_GPU_TEXTURETYPE_2D, SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET)) {
        if (const char* ve = getenv("MD_SPV_VARIANTS")) {   // measurement (#207 variant_eval): one more pass-B pipeline per directory of the comma list, all created now (no restart per variant)
            std::string list(ve); size_t pos = 0; var_count_ = 0;
            while (pos <= list.size() && var_count_ < 8) {
                size_t e = list.find(',', pos); if (e == std::string::npos) e = list.size();
                const std::string dir = list.substr(pos, e - pos); pos = e + 1; if (dir.empty()) continue;
                const int vi = var_count_++;
                var_names_[vi] = "variant:" + std::to_string(vi) + ":" + dir;
                const std::string spv = dir + "/terrain_shading_screenspace_kenshi_klight.frag.spv";
                for (int f = 0; f < 2; ++f) {
                    GpuPipeline::Desc vd = rd;
                    vd.color_format = f ? SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT : rd.color_format; vd.depth_format = SDL_GPU_TEXTUREFORMAT_D32_FLOAT_S8_UINT;
                    static const int ub_bid_v = getenv("MD_UBIOME_BID") ? atoi(getenv("MD_UBIOME_BID")) : -1;
                    vd.frag_uniform_bufs = ub_bid_v >= 0 ? 4 : 3; vd.frag_path = var_names_[vi].c_str(); vd.frag_spv_path = spv.c_str(); vd.stencil_mode = VariantEnvBit("MD_VARIANT_NOSTENCIL", vi) ? 0 : 2;   // H11c: no stencil test = the variant draws every pixel
                    fprintf(stderr, "[VariantPipe] %d %d %s\n", vi, f, dir.c_str());
                    if (!resolve_var_p_[vi][f].Create(vd)) MD_LOG(MD_LOG_WARNING, "[TerrainShadingProjected] variant %d (%s) failed", vi, dir.c_str());
                }
            }
            MD_LOG(MD_LOG_INFO, "[TerrainShadingProjected] pass-B variants: %d", var_count_);
        }
        }
    }
    // #140 L2: the Kenshi main light variants (cube maps in slots 5 / 6, KLightUBO at binding 2); non-fatal
    {
        GpuPipeline::Desc kd = rd;
        kd.frag_path = "shaders/terrain_shading_screenspace_kenshi_klight.frag";
        resolve_klight_ready_ = resolve_kenshi_klight_pipeline_.Create(kd);
        kd.frag_path = "shaders/terrain_shading_screenspace_kenshi_klight.frag";
        kd.frag_uniform_bufs = 4;   // + the reuse UBO at 3
        kd.frag_path = "shaders/terrain_shading_screenspace_kenshi_reuse_klight.frag";
        resolve_reuse_klight_ready_ = resolve_kenshi_reuse_klight_pipeline_.Create(kd);
        // #140 L4: the same two with the RGBA16F colour target (the ground target before the exposure)
        kd.color_format = SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT;
        resolve_reuse_klight_f_ready_ = resolve_reuse_klight_f_pipeline_.Create(kd);
        kd.frag_uniform_bufs = 3; kd.frag_path = "shaders/terrain_shading_screenspace_kenshi_klight.frag";
        resolve_klight_f_ready_ = resolve_klight_f_pipeline_.Create(kd);
        if (!resolve_klight_f_ready_ || !resolve_reuse_klight_f_ready_) MD_LOG(MD_LOG_WARNING, "[TerrainShadingProjected] float-target klight variants failed (L4 unavailable)");
        if (getenv("MD_UBIOME_PROBE")) {   // measurement only (#207 H1): create the per-draw-biome variants so the compiler dump (INTEL_DEBUG=fs) shows them; they are never drawn
            kd.frag_uniform_bufs = 4; kd.frag_path = "shaders/terrain_shading_screenspace_kenshi_ubiome_klight.frag";
            const bool a = ubiome_probe_p_[0].Create(kd);
            kd.frag_path = "shaders/terrain_shading_screenspace_kenshi_ubiome_mat.frag"; kd.color_format2 = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
            const bool b = ubiome_probe_p_[1].Create(kd);
            MD_LOG(MD_LOG_INFO, "[TerrainShadingProjected] uniform-biome probe pipelines: material+klight %d, material %d", (int)a, (int)b);
        }
        if (const char* e = getenv("MD_STENCIL_SPLIT"); !(e && e[0] == '0')) {   // #207: the ground resolve as two stencil-separated passes (default on, MD_STENCIL_SPLIT=0 turns it off): the lean reuse-only pass marks the accepted pixels, the heavy pass runs on the others
            SDL_GPUDevice* sdev = md::GpuDevice::Get().SDLDevice();
            if (sdev && SDL_GPUTextureSupportsFormat(sdev, SDL_GPU_TEXTUREFORMAT_D32_FLOAT_S8_UINT, SDL_GPU_TEXTURETYPE_2D, SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET)) {
                bool all_ok = true;
                for (int f = 0; f < 2; ++f) {   // f = 0: the default RGBA8 ground target (lighting mode 2), f = 1: the RGBA16F one (mode 4)
                    GpuPipeline::Desc sd = kd;
                    sd.color_format = f ? SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT : rd.color_format;
                    sd.depth_format = SDL_GPU_TEXTUREFORMAT_D32_FLOAT_S8_UINT;
                    sd.frag_uniform_bufs = 4; sd.frag_path = "shaders/terrain_shading_screenspace_kenshi_reuse_only_klight.frag"; sd.stencil_mode = 1;
                    all_ok &= resolve_split_p_[f][0].Create(sd);
                    static const int ub_bid = getenv("MD_UBIOME_BID") ? atoi(getenv("MD_UBIOME_BID")) : -1;   // measurement (#207 H1 step 3): the heavy passes get a 4th uniform block with this biome's 9 parameter vec4s (needs the TS_UNIFORM_BIOME SPIR-V through MD_SPV_OVERRIDE)
                    sd.frag_uniform_bufs = ub_bid >= 0 ? 4 : 3; sd.frag_path = "shaders/terrain_shading_screenspace_kenshi_klight.frag"; sd.stencil_mode = 2;
                    all_ok &= resolve_split_p_[f][1].Create(sd);
                    sd.frag_path = "shaders/terrain_shading_screenspace_kenshi_onebiome_klight.frag"; sd.stencil_mode = 3;   // the pixels with exactly one biome evaluation (the others are discarded here and shaded by the pass above)
                    all_ok &= resolve_split_p_[f][2].Create(sd);
                }
                split_ready_ = all_ok;
                MD_LOG(MD_LOG_INFO, "[TerrainShadingProjected] stencil split pipelines ok=%d", (int)all_ok);
                if (all_ok) if (const char* pe = getenv("MD_TERRAIN_PVT"); pe && pe[0] == '1') {   // #207 PVT stage 1
                    const char* me = getenv("MD_PVT_MB");
                    bool ok = pvt_.Init(sdev, me ? atoi(me) : 64);
                    for (int f = 0; f < 2; ++f) {
                        GpuPipeline::Desc pd = kd;
                        pd.color_format = f ? SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT : rd.color_format;
                        pd.depth_format = SDL_GPU_TEXTUREFORMAT_D32_FLOAT_S8_UINT;
                        pd.frag_uniform_bufs = 3; pd.frag_path = "shaders/terrain_shading_screenspace_kenshi_pvt_klight.frag"; pd.stencil_mode = getenv("MD_PVT_MARK") ? 2 : (getenv("MD_PVT_SEPMODE") ? atoi(getenv("MD_PVT_SEPMODE")) : 3);   // MD_PVT_SEPMODE: measurement only (2 = no stencil write: tests whether the stencil write + discard forces a late stencil test)
                        ok &= resolve_pvt_p_[f].Create(pd);
                        if (getenv("MD_PVT_MARK")) {   // measurement (#207 marker pass): the shading pass above runs in stencil mode 2 (early reject, no write); this cheap pass marks the hits (stencil write, colour mask 0, discard on a miss) so pass B skips them
                            GpuPipeline::Desc mk = pd; mk.stencil_mode = 3; mk.raster.color_write = false;
                            mk.frag_path = "shaders/terrain_shading_screenspace_kenshi_pvt_mark_klight.frag";
                            ok &= resolve_pvt_mark_p_[f].Create(mk);
                        }
                        pd.frag_uniform_bufs = 4; pd.frag_path = "shaders/terrain_shading_screenspace_kenshi_pvt_a_klight.frag";   // PVT lookup inside pass A: no extra full-screen pass
                        pd.stencil_mode = 1;
                        ok &= resolve_pvt_a_p_[f].Create(pd);
                    }
                    GpuPipeline::Desc td = kd;
                    td.frag_path = "shaders/terrain_shading_screenspace_kenshi_pvt_tile.frag"; td.frag_uniform_bufs = 4;
                    td.color_format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM_SRGB; td.color_format2 = SDL_GPU_TEXTUREFORMAT_R8G8_UNORM;
                    td.has_depth_target = false; td.depth_format = SDL_GPU_TEXTUREFORMAT_INVALID; td.stencil_mode = 0;
                    ok &= pvt_tile_p_.Create(td);
                    pvt_ready_ = ok;
                    MD_LOG(MD_LOG_INFO, "[TerrainShadingProjected] PVT pipelines ok=%d", (int)ok);
                }
            } else MD_LOG(MD_LOG_WARNING, "[TerrainShadingProjected] stencil split off: D32_FLOAT_S8_UINT is not a supported depth-stencil target");
        }
    }
    ready_ = true;
    MD_LOG(MD_LOG_INFO, "[TerrainShadingProjected] ready %dx%d (RGBA32F gbuffer + isolated D32_FLOAT)", w, h);
    return true;
}

// Only the two render-target textures depend on window size -- resolve_kenshi_pipeline_
// depends solely on texture FORMAT (unchanged across a resize), so it must never
// be touched here. The original version of this function called the full Init()
// (destroying+recreating the pipeline too), which meant every resize destroyed a
// GPU pipeline object that a still-in-flight command buffer from the previous
// frame could still reference -- a real SIGSEGV inside the Intel Vulkan driver,
// diagnosed live 2026-08-02 even after relocating the call to before
// AcquireCommandBuffer (that relocation alone only fixed the FIRST-frame case).
void TerrainShadingProjected::EnsureSize(md::GpuDeviceHandle dev, int w, int h) {
    (void)dev;
    xs_tex_ = nullptr; xs_samp_ = nullptr;
    if (!ready_ || (w == w_ && h == h_) || w <= 0 || h <= 0) return;
    if (hist_on_ && gd_prev_->SDLTexture()) { xs_tex_ = gd_prev_->SDLTexture(); xs_samp_ = gd_prev_->SDLSampler(); }
    if (!Cached(w, h)) {   // a size not seen before: the oldest set goes (the caller drained the GPU), the current one becomes the alt set
        alt_hist_.Shutdown(); alt_depth_.Shutdown(); alt_color_.Shutdown();
        std::swap(gbuf_color_, alt_color_); std::swap(gbuf_depth_, alt_depth_); std::swap(gd_alt_, alt_hist_);
        alt_w_ = w_; alt_h_ = h_;
        CreateTextures(w, h);
    } else {
        std::swap(gbuf_color_, alt_color_); std::swap(gbuf_depth_, alt_depth_); std::swap(gd_alt_, alt_hist_);
        std::swap(w_, alt_w_); std::swap(h_, alt_h_);
    }
    gd_cur_ = &gbuf_depth_; gd_prev_ = &gd_alt_;
    if (hist_on_ && !gd_alt_.SDLTexture()) gd_alt_.Init(w, h, /*shadow_border=*/false);
    if (!hist_on_ && gd_alt_.SDLTexture()) gd_alt_.Shutdown();
}

void TerrainShadingProjected::EnsureHistory(bool on, int w, int h) {
    if (!ready_) return;
    hist_on_ = on;
    if (on && !gd_alt_.SDLTexture()) gd_alt_.Init(w, h, /*shadow_border=*/false);
    if (!on && gd_alt_.SDLTexture()) { gd_alt_.Shutdown(); gd_cur_ = &gbuf_depth_; gd_prev_ = &gd_alt_; }
}

void TerrainShadingProjected::Shutdown() {
    resolve_kenshi_pipeline_.Destroy();
    resolve_kenshi_ready_ = false;
    resolve_kenshi_noskip_pipeline_.Destroy();
    resolve_kenshi_noskip_ready_ = false;
    resolve_kenshi_reuse_pipeline_.Destroy();
    resolve_kenshi_reuse_ready_ = false;
    resolve_kenshi_klight_pipeline_.Destroy(); resolve_kenshi_reuse_klight_pipeline_.Destroy();
    resolve_klight_f_pipeline_.Destroy(); resolve_reuse_klight_f_pipeline_.Destroy();
    for (auto& sp : resolve_split_p_) for (GpuPipeline& q : sp) q.Destroy(); split_ready_ = false;
    for (auto& vp : resolve_var_p_) for (GpuPipeline& q : vp) q.Destroy(); var_count_ = 0;
    for (GpuPipeline& q : resolve_pvt_p_) q.Destroy();
    for (GpuPipeline& q : resolve_pvt_mark_p_) q.Destroy();
    for (GpuPipeline& q : resolve_pvt_a_p_) q.Destroy();
    for (GpuPipeline& q : ubiome_probe_p_) q.Destroy();
    pvt_tile_p_.Destroy(); pvt_.Shutdown(md::GpuDevice::Get().SDLDevice()); pvt_ready_ = pvt_active_ = false;
    resolve_klight_ready_ = resolve_reuse_klight_ready_ = false;
    gd_alt_.Shutdown();
    gbuf_depth_.Shutdown();
    gbuf_color_.Shutdown();
    alt_hist_.Shutdown(); alt_depth_.Shutdown(); alt_color_.Shutdown(); alt_w_ = alt_h_ = 0;
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
    cpd.depth_tex      = gd_cur_->SDLTexture();
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

void TerrainShadingProjected::DrawFullscreen(SDL_GPURenderPass* rp, md::GpuCommandBufferHandle cmd, GpuPipeline& pipeline, const ResolveCtx& c) {
    GpuPassView pv = GpuPassView::FromRaw(rp, cmd);
    pv.BindPipeline(&pipeline);
    const TerrainRenderer::SunParams& sun = *c.sun;
    const TerrainRenderer& ground = *c.ground;
    const float cam_x = c.cam_x, cam_y = c.cam_y, cam_z = c.cam_z, world_origin_x = c.world_origin_x, world_origin_z = c.world_origin_z, world_to_uv = c.world_to_uv;
    const float fog_far = c.fog_far, fog_near = c.fog_near; const float* fog_color = c.fog_color;

    ProjFragUBO fubo{};
    fubo.sun_dir_str[0] = sun.dir[0]; fubo.sun_dir_str[1] = sun.dir[1];
    fubo.sun_dir_str[2] = sun.dir[2]; fubo.sun_dir_str[3] = sun.strength;
    fubo.ambient[0]     = sun.ambient[0]; fubo.ambient[1] = sun.ambient[1];
    fubo.ambient[2]     = sun.ambient[2];
    fubo.ambient[3]     = units10_ ? 1.f : 0.f; // .w = "terrain_units10" flag (the shader's ambient uses .xyz only)
    fubo.world_params[0] = world_origin_x; fubo.world_params[1] = world_origin_z;
    fubo.world_params[2] = world_to_uv;
    fubo.world_params[3] = 2.f; // the Kenshi-parity ground path (shader: world_params.w > 1.5)
    fubo.fog_color_near[0] = fog_color[0]; fubo.fog_color_near[1] = fog_color[1];
    fubo.fog_color_near[2] = fog_color[2]; fubo.fog_color_near[3] = fog_near;
    fubo.fog_far = fog_far;
    GpuPushFragmentUniforms(cmd, 0, &fubo, sizeof(fubo));

    ProjCamUBO cubo{};
    cubo.cam_pos_ws[0] = cam_x; cubo.cam_pos_ws[1] = cam_y;
    cubo.cam_pos_ws[2] = cam_z; cubo.cam_pos_ws[3] = (g_terrain_ambientmap && ground.AmbientMapReady())
        ? (pix_scale_ < 0.f ? pix_scale_ - 1600.f : pix_scale_ + 1600.f) : pix_scale_; // bit4 = ambientmap (see g_terrain_ambientmap)
    std::memcpy(cubo.inv_vp, s7_.inv_vp, sizeof(cubo.inv_vp));   // recon (world extent / resolution / mode of the S7 path) is read by no shader since the 8 B G-buffer
    cubo.recon[0] = cubo.recon[1] = cubo.recon[2] = cubo.recon[3] = 0.f;
    GpuPushFragmentUniforms(cmd, 1, &cubo, sizeof(cubo));
    {   // #167 P0: the biome table for the shader variants that read it as a uniform block (128 biomes x 9 uvec4; the rest zero)
        constexpr size_t kUboWords = 128 * 9 * 4;
        std::vector<uint32_t> bp(kUboWords, 0u);
        const std::vector<uint32_t>& src = ground.BiomeLayersData();
        std::memcpy(bp.data(), src.data(), std::min(src.size(), kUboWords) * sizeof(uint32_t));
        { static bool once = false; if (!once) { once = true; fprintf(stderr, "[P0dbg] biome words %zu first %u %u %u %u idx32 %u\n", src.size(), src.size() > 3 ? src[0] : 0u, src.size() > 3 ? src[1] : 0u, src.size() > 3 ? src[2] : 0u, src.size() > 3 ? src[3] : 0u, src.size() > 32 ? src[32] : 0u); } }
        GpuPushFragmentUniforms(cmd, 2, bp.data(), bp.size() * sizeof(uint32_t));
    }

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
        if (VariantNearMip(g_ground_variant)) if (SDL_GPUSampler* nm = GroundNearMipSampler()) { ground_bindings[1].sampler = nm; ground_bindings[4].sampler = nm; }
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
    const bool reuse_draw = (&pipeline == &resolve_kenshi_reuse_pipeline_ || &pipeline == &resolve_kenshi_reuse_klight_pipeline_ || &pipeline == &resolve_reuse_klight_f_pipeline_ || &pipeline == &resolve_split_p_[0][0] || &pipeline == &resolve_split_p_[1][0] || &pipeline == &resolve_pvt_a_p_[0] || &pipeline == &resolve_pvt_a_p_[1]) && reuse_;
    const bool klight_draw = (&pipeline == &resolve_kenshi_klight_pipeline_ || &pipeline == &resolve_kenshi_reuse_klight_pipeline_ || &pipeline == &resolve_klight_f_pipeline_ || &pipeline == &resolve_reuse_klight_f_pipeline_ || (&pipeline >= &resolve_split_p_[0][0] && &pipeline <= &resolve_split_p_[1][2]) || (&pipeline >= &resolve_var_p_[0][0] && &pipeline <= &resolve_var_p_[7][1]) || &pipeline == &pvt_tile_p_ || &pipeline == &resolve_pvt_p_[0] || &pipeline == &resolve_pvt_p_[1] || &pipeline == &resolve_pvt_a_p_[0] || &pipeline == &resolve_pvt_a_p_[1]) && klight_;
    if (klight_draw) {   // slots 5 / 6: the Kenshi cube maps
        ground_bindings[5] = { ground.KenshiIrradianceCube(), ground.KenshiIrradianceSampler() };
        ground_bindings[6] = { ground.KenshiSpecularityCube(), ground.KenshiSpecularitySampler() };
        pv.PushFragmentUniforms(2, klight_->sun_dir, sizeof(KLightParams));
    }
    if (reuse_draw) ground_bindings[2] = xs_tex_ ? SDL_GPUTextureSamplerBinding{ xs_tex_, xs_samp_ } : SDL_GPUTextureSamplerBinding{ gd_prev_->SDLTexture(), gd_prev_->SDLSampler() };   // slot 2 of the reuse variant: the previous G-buffer depth
    if (c.pvt == 2) {   // the atlas stands in for the ground arrays (slots 1 / 4); the page table takes the biome-layers slot below
        ground_bindings[1] = { pvt_.Albedo(), pvt_.Linear() };
        ground_bindings[4] = { pvt_.Normal(), pvt_.Linear() };
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
        { gd_cur_->SDLTexture(), gd_cur_->SDLSampler() },
    };
    if (c.pvt == 1) {   // the tile bake reads the fine normal window (xyz normal, w height) where the resolve reads the depth
        gbuf_bindings[1] = { pvt_fine_.norm_tex, pvt_.Linear() };
        pv.PushFragmentUniforms(3, c.pvt_ubo, 32);
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
    if (c.pvt == 2) kenshi_bindings[2] = { pvt_.Table(), pvt_.Nearest() };   // the page table stands in for biomeLayersTex
    pv.BindFragmentSamplers(11, kenshi_bindings, 3);

    // Divergence #14: binding=14, Kenshi's ambientmap.png. Falls back to the (always loaded) biome-blend texture as a type-matching
    // dummy when the file is missing -- the shader only reads it when bit4 of |pix_scale| is set, which needs AmbientMapReady().
    SDL_GPUTextureSamplerBinding ambient_binding[1] = {
        ground.AmbientMapReady() ? SDL_GPUTextureSamplerBinding{ ground.AmbientMapTexture(), ground.AmbientMapSampler() }
                                 : SDL_GPUTextureSamplerBinding{ ground.BiomeBlendTexture(), ground.BiomeBlendSampler() },
    };
    pv.BindFragmentSamplers(14, ambient_binding, 1);

    // binding 15: the history colour of the reuse variants; gbuf_color_ is the same format and stands in for the other variants
    SDL_GPUTextureSamplerBinding slot15[1] = { SDL_GPUTextureSamplerBinding{ gbuf_color_.SDLTexture(), gbuf_color_.SDLSampler() } };
    {   // measurement (#207 H1 step 3): the one-biome parameters of the whole draw as a uniform block
        static const int ub_bid = getenv("MD_UBIOME_BID") ? atoi(getenv("MD_UBIOME_BID")) : -1;
        const std::vector<uint32_t>& bl = ground.BiomeLayersData();
        if (ub_bid >= 0 && (&pipeline == &resolve_split_p_[0][1] || &pipeline == &resolve_split_p_[1][1] || &pipeline == &resolve_split_p_[0][2] || &pipeline == &resolve_split_p_[1][2] || (&pipeline >= &resolve_var_p_[0][0] && &pipeline <= &resolve_var_p_[7][1])) && bl.size() >= (size_t)(ub_bid + 1) * 36)
            pv.PushFragmentUniforms(3, bl.data() + (size_t)ub_bid * 36, 36 * sizeof(uint32_t));
    }
    if (reuse_draw) { slot15[0] = { reuse_->hist_colour, reuse_->hist_sampler }; pv.PushFragmentUniforms(3, reuse_->p_old, 224); }   // p_old, p_new, t (3 x 64 B) + params (16 B), in the order of ReuseParams
    pv.BindFragmentSamplers(15, slot15, 1);

    // 2026-09-19 (docs/RESOLVE_OPT.md session finding): directional
    // ambient via AmbientProbeSystem, binding=14 (2026-09-26: was 11,
    // shifted +3 after adding the 3 Kenshi-parity samplers above) --
    // see terrain_shading_common.glsl's TS_HAS_AMBIENT_PROBE doc comment.
    SDL_GPUBuffer* ambient_probe_buf = AmbientProbeSystem::Get().GetSSBO().SDLBuffer();
    SDL_GPUBuffer* storage_bufs[2] = { ambient_probe_buf, ground.BiomeParamsBuffer() ? ground.BiomeParamsBuffer() : ambient_probe_buf };
    pv.BindFragmentStorageBuffers(0, storage_bufs, 2);

    pv.Draw(3, 1, 0, 0);
}

void TerrainShadingProjected::DrawShadingResolve(SDL_GPURenderPass* rp, md::GpuCommandBufferHandle cmd,
                                                   const TerrainRenderer::SunParams& sun,
                                                   float cam_x, float cam_y, float cam_z,
                                                   float world_origin_x, float world_origin_z, float world_to_uv,
                                                   float fog_far, const float fog_color[3], float fog_near,
                                                   const TerrainRenderer& ground) {
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
    c.ground = &ground;
    // RESOLVE_OPT 4-draw split reverted to a single draw -- see
    // resolve_kenshi_pipeline_'s own doc comment (terrain_shading_projected.h)
    // for the full measured rationale.
    GpuPipeline* kp = &resolve_kenshi_pipeline_;
    if (!skip_flat_on_cliff_ && resolve_kenshi_noskip_ready_) kp = &resolve_kenshi_noskip_pipeline_;
    const bool use_reuse = reuse_ && resolve_kenshi_reuse_ready_ && HistoryReady() && reuse_->hist_colour;
    const bool use_klight = klight_ && ground.KenshiLightCubesReady();
    if (use_reuse) kp = (use_klight && resolve_reuse_klight_ready_) ? &resolve_kenshi_reuse_klight_pipeline_ : &resolve_kenshi_reuse_pipeline_;
    else if (use_klight && resolve_klight_ready_) kp = &resolve_kenshi_klight_pipeline_;
    if (float_target_ && use_klight) {
        if (use_reuse && resolve_reuse_klight_f_ready_) kp = &resolve_reuse_klight_f_pipeline_;
        else if (!use_reuse && resolve_klight_f_ready_) kp = &resolve_klight_f_pipeline_;
    }
    if (split_active_) {   // #207: the pass has a depth-stencil attachment, only pipelines of that format may draw in it
        if (!(split_ready_ && use_klight)) { static bool once = false; if (!once) { once = true; MD_LOG(MD_LOG_WARNING, "[TerrainShadingProjected] stencil split pass without klight / float target: ground not drawn"); } return; }
        SDL_SetGPUStencilReference(rp, 1);
        static const bool pvt_sep = getenv("MD_PVT_SEPARATE") != nullptr;   // measurement: the old separate PVT pass
        const bool pvt_in_a = use_reuse && pvt_active_ && !pvt_sep;   // #207 PVT: with reuse the cache lookup is part of pass A (accepted pixel, else resident tile, else discard to pass B)
        const bool h11_skip = g_ground_variant >= 0 && VariantEnvBit("MD_VARIANT_SKIPALL", g_ground_variant);   // H11d: begin / end the pass with no draw at all
        if (h11_skip) return;
        if (use_reuse) {   // pass A (lean): marks the accepted pixels; then the one-biome pass and the full pass run on the others only (stencil test)
            if (pvt_in_a) { ResolveCtx pc = c; pc.pvt = 2; DrawFullscreen(rp, cmd, resolve_pvt_a_p_[float_target_ ? 1 : 0], pc); }
            else DrawFullscreen(rp, cmd, resolve_split_p_[float_target_ ? 1 : 0][0], c);
        }
        if (pvt_active_ && !pvt_in_a) { ResolveCtx pc = c; pc.pvt = 2; DrawFullscreen(rp, cmd, resolve_pvt_p_[float_target_ ? 1 : 0], pc); }   // #207 PVT: the pixels whose tile is resident (the others are left to the full pass)
        if (pvt_active_ && !pvt_in_a && getenv("MD_PVT_MARK")) { ResolveCtx pc = c; pc.pvt = 2; DrawFullscreen(rp, cmd, resolve_pvt_mark_p_[float_target_ ? 1 : 0], pc); }   // measurement: mark the hits after the shading pass
        static const bool one_biome_pass = getenv("MD_ONEBIOME_PASS") && getenv("MD_ONEBIOME_PASS")[0] == '1';   // WIP: off until it is verified (0 px on the 17 views) and measured; MD_ONEBIOME_PASS=1 turns it on
        if (one_biome_pass) DrawFullscreen(rp, cmd, resolve_split_p_[float_target_ ? 1 : 0][2], c);
        if (g_ground_variant >= 0 && g_ground_variant < var_count_) DrawFullscreen(rp, cmd, resolve_var_p_[g_ground_variant][float_target_ ? 1 : 0], c);   // measurement: the pass-B variant of the frame block
        else DrawFullscreen(rp, cmd, resolve_split_p_[float_target_ ? 1 : 0][1], c);
        return;
    }
    DrawFullscreen(rp, cmd, *kp, c);
}

void TerrainShadingProjected::PvtFrame(md::GpuCommandBufferHandle cmd, const TerrainRenderer::SunParams& sun, float cam_x, float cam_y, float cam_z,
                                       float world_origin_x, float world_origin_z, float world_to_uv, float fog_far, const float fog_color[3], float fog_near,
                                       const TerrainRenderer& ground, const PvtFine& fine, uint32_t frame, bool float_target) {
    pvt_active_ = false;
    if (!pvt_ready_ || !ready_ || !klight_ || !fine.ok || !fine.norm_tex || !pvt_hf_ || !ground.KenshiLightCubesReady() || h_ <= 0 || pix_scale_ <= 0.f) return;
    pvt_fine_ = fine;
    const float cam[3] = { cam_x, cam_y, cam_z };
    const float margin = 2.f * fine.spacing;
    const float fmin[2] = { fine.ox * fine.spacing + margin, fine.oz * fine.spacing + margin };
    const float fmax[2] = { (fine.ox + fine.side - 1) * fine.spacing - margin, (fine.oz + fine.side - 1) * fine.spacing - margin };
    static const int budget = [] { const char* e = getenv("MD_PVT_BAKE"); return e ? std::max(0, std::min(64, atoi(e))) : 8; }();
    static const uint32_t freeze = [] { const char* e = getenv("MD_PVT_FREEZE"); return e ? (uint32_t)std::max(0, atoi(e)) : 0u; }();   // measurement only: no baking from this frame on (bake cost = A/B against the same warm atlas)
    TerrainPvt::Bake bk[64];
    const float ps_abs = std::fabs(pix_scale_), ps = ps_abs - 100.f * std::floor((ps_abs + 50.f) / 100.f);   // the flags ride on pix_scale as +100 k (the shader decodes them the same way)
    const int nb = pvt_.Plan(cam, s7_.inv_vp, ps / (float)h_, pvt_hf_, fmin, fmax, frame, bk, (freeze && frame >= freeze) ? 0 : budget);
    pvt_.UploadTable(md::GpuDevice::Get().SDLDevice(), cmd);

    ResolveCtx c;
    c.sun = &sun; c.cam_x = cam_x; c.cam_y = cam_y; c.cam_z = cam_z;
    c.world_origin_x = world_origin_x; c.world_origin_z = world_origin_z; c.world_to_uv = world_to_uv;
    c.fog_far = fog_far; c.fog_color = fog_color; c.fog_near = fog_near; c.ground = &ground;
    c.pvt = 1;
    for (int i = 0; i < nb; ++i) {
        const float T = TerrainPvt::TexelOf(bk[i].mip), S = (float)TerrainPvt::kInner * T;
        float ubo[8] = { bk[i].ti * S - TerrainPvt::kBorder * T, bk[i].tj * S - TerrainPvt::kBorder * T, T, T / TerrainPvt::kRefPixAngle,
                         fine.ox * fine.spacing, fine.oz * fine.spacing, 1.f / fine.spacing, (float)fine.side };
        c.pvt_ubo = ubo;
        SDL_GPUColorTargetInfo ct[2] = {};
        ct[0].texture = pvt_.Albedo(); ct[1].texture = pvt_.Normal();
        for (auto& t : ct) { t.layer_or_depth_plane = (Uint32)bk[i].layer; t.load_op = SDL_GPU_LOADOP_DONT_CARE; t.store_op = SDL_GPU_STOREOP_STORE; }
        SDL_GPURenderPass* rp = SDL_BeginGPURenderPass(cmd, ct, 2, nullptr);
        if (!rp) continue;
        DrawFullscreen(rp, cmd, pvt_tile_p_, c);
        SDL_EndGPURenderPass(rp);
    }
    pvt_active_ = pvt_.GetStats().resident > 0;
    static const bool log = getenv("MD_PVT_LOG") && getenv("MD_PVT_LOG")[0] == '1';
    if (log && (frame % 120) == 0) { const auto& st = pvt_.GetStats(); MD_LOG(MD_LOG_INFO, "[PVT] frame %u wanted %d missing %d resident %d/%d baked %d (+%d)", frame, st.wanted, st.missing, st.resident, st.layers, st.baked_total, nb); }
    (void)float_target;
}

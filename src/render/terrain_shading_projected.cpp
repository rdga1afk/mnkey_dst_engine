#ifdef MD_SDL_GPU
#include <monkey_dust/render/terrain_shading_projected.h>
#include <monkey_dust/render/gpu_device.h>
#include <monkey_dust/render/gpu_hal_free_functions.h>
#include <monkey_dust/render/ambient_probe.h>
#include <monkey_dust/platform/md_log.h>
#include <cstring>

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
};
static_assert(sizeof(ProjCamUBO) == 16, "ProjCamUBO size mismatch");

bool TerrainShadingProjected::CreateTextures(int w, int h) {
    w_ = w; h_ = h;

    GpuSamplerDesc gs;
    gs.min_filter = GpuSamplerDesc::Filter::NEAREST;
    gs.mag_filter = GpuSamplerDesc::Filter::NEAREST;
    gs.wrap_s = GpuSamplerDesc::Wrap::CLAMP_TO_EDGE;
    gs.wrap_t = GpuSamplerDesc::Wrap::CLAMP_TO_EDGE;
    if (!gbuf_color_.InitRenderTarget(w, h, gs, SDL_GPU_TEXTUREFORMAT_R32G32B32A32_FLOAT)) {
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
    rd.frag_samplers      = 14; // set=2: tex_colour,tex_ground,tex_ground_baked,tex_overlay_mask,tex_ground_nml (task #12), tex_detail_array,tex_detail_tint (КРОК3 2026-09-17); gbufPacked,gbufDepth; zoneGroundLayersTex (texture, not SSBO, since 2026-08-09); texSteepnessSmoothed (2026-09-24, bake/live cliff_w single-source-of-truth); tex_biome_blend/kbi1LookupTex/biomeLayersTex (task-terrain-kenshi-parity, 2026-09-26). БОРГ-TERRAIN-2 (2026-09-13): was 10 -- vtIndirection/vtAtlas removed, dead VT cache-hit path never called. 2026-09-26: was 11 -- task #141's zone-corner cliff bake atlas (3 samplers) removed then task-terrain-kenshi-parity's 3 samplers added back, see CLAUDE_HISTORY.md.
    // 2026-09-19 (docs/RESOLVE_OPT.md session finding): AmbientProbeBuf,
    // binding=11 (2026-09-26: was 14, shifted -3 after removing the
    // corner-bake atlas's 3 samplers; after the 11 samplers 0-10 above) --
    // see terrain_shading_common.glsl's TS_HAS_AMBIENT_PROBE doc comment.
    // Was 0 since БОРГ-TERRAIN-2 (2026-09-13, vtPageMeta removed with TerrainVtPageCache).
    rd.frag_storage_bufs  = 1;
    if (!resolve_pipeline_.Create(rd)) {
        MD_LOG(MD_LOG_WARNING, "[TerrainShadingProjected] resolve pipeline create failed");
        return false;
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
    auto drawOne = [&](GpuPipeline& pipeline) {
        GpuPassView pv = GpuPassView::FromRaw(rp, cmd);
        pv.BindPipeline(&pipeline);

        ProjFragUBO fubo{};
        fubo.sun_dir_str[0] = sun.dir[0]; fubo.sun_dir_str[1] = sun.dir[1];
        fubo.sun_dir_str[2] = sun.dir[2]; fubo.sun_dir_str[3] = sun.strength;
        fubo.ambient[0]     = sun.ambient[0]; fubo.ambient[1] = sun.ambient[1];
        fubo.ambient[2]     = sun.ambient[2]; fubo.ambient[3] = 0.f;
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
        cubo.cam_pos_ws[2] = cam_z; cubo.cam_pos_ws[3] = 0.f;
        GpuPushFragmentUniforms(cmd, 1, &cubo, sizeof(cubo));

        // set=2: same 7 shared ground samplers the normal forward terrain draw
        // binds (TerrainPatchRenderer::DrawBatch) -- index 4 (tex_ground_nml)
        // added task #12 (2026-09-03); index 5/6 (КРОК 3 packed detail array +
        // tint) added 2026-09-17.
        SDL_GPUTextureSamplerBinding ground_bindings[7];
        ground.GetSharedGroundSamplers(ground_bindings);
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
        SDL_GPUTextureSamplerBinding gbuf_bindings[2] = {
            { gbuf_color_.SDLTexture(), gbuf_color_.SDLSampler() },
            { gbuf_depth_.SDLTexture(), gbuf_depth_.SDLSampler() },
        };
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

        // 2026-09-19 (docs/RESOLVE_OPT.md session finding): directional
        // ambient via AmbientProbeSystem, binding=14 (2026-09-26: was 11,
        // shifted +3 after adding the 3 Kenshi-parity samplers above) --
        // see terrain_shading_common.glsl's TS_HAS_AMBIENT_PROBE doc comment.
        SDL_GPUBuffer* ambient_probe_buf = AmbientProbeSystem::Get().GetSSBO().SDLBuffer();
        pv.BindFragmentStorageBuffers(0, &ambient_probe_buf, 1);

        pv.Draw(3, 1, 0, 0);
    };

    // RESOLVE_OPT 4-draw split reverted to a single draw -- see
    // resolve_pipeline_'s own doc comment (terrain_shading_projected.h)
    // for the full measured rationale.
    drawOne(resolve_pipeline_);
}
#endif

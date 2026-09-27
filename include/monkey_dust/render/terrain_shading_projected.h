#pragma once
#ifdef MD_SDL_GPU
#include <monkey_dust/render/gpu_pipeline.h>
#include <monkey_dust/render/gpu_depth_texture.h>
#include <monkey_dust/render/gpu_sampler_texture.h>
#include <monkey_dust/render/gpu_pass_view.h>
#include <monkey_dust/render/terrain_renderer.h>
#include <SDL3/SDL_gpu.h>

// TERRAIN_CA_REBUILD_PROMPT.md Phase 4 -- Variant A (screen-space decoupled
// shading, terrain_research/perf/PHASE4_PLAN.md). Owns the two resources
// TerrainPatchRenderer::DrawBatchGBuffer writes into and the fullscreen
// resolve pass that reads them back:
//
//   1. gbuf_color_ -- RGBA32F render target. terrain_gbuffer_mini.frag packs
//      world-pos.xyz into .xyz and normal.xz (16-bit fixed point) into .w
//      (see that shader's doc comment for why a single packed target instead
//      of true multi-render-target -- GpuPipeline's HAL has no MRT support,
//      and the one prior 2-target G-buffer pipeline in this codebase
//      (npc_render.cpp's gbuffer_pipeline_) is dead/unused code, not a
//      working precedent worth extending the HAL for on an experimental,
//      likely-losing path).
//   2. gbuf_depth_ -- a DEDICATED, ISOLATED D32_FLOAT depth target, NOT the
//      shared NPC/scene `depth` (npc_render.h). That shared depth already
//      carries the real terrain depth from the existing Early-Z prepass
//      (TERRAIN_CA_REBUILD_PROMPT.md Phase 2 §3); this pass needs its OWN
//      depth purely to resolve which terrain instance/patch wins at each
//      screen pixel among overlapping draws, cleared to 1.0 (= "no terrain
//      here", the resolve pass's discard mask). Changing the shared depth's
//      format to something stencil-capable so a mask could live there
//      instead would touch every consumer of it (NPC prepass, both terrain
//      paths, sky, water, HUD) for one experimental variant -- rejected as
//      disproportionate blast radius; an isolated texture is zero risk to
//      the rest of the renderer.
//   3. resolve_pipeline_ -- fullscreen triangle (gl_VertexIndex, no vertex
//      buffer, same pattern as smaa_edge.vert), draws INSIDE the caller's
//      existing main color GpuRenderPass (same call site DrawBatch/DrawSlot
//      normally occupies in NpcRender::DrawTerrainAndProps) so ordinary
//      time-ordering against NPCs/props falls out for free -- BUT since a
//      fullscreen triangle has no real per-pixel geometric depth of its own,
//      the fragment shader manually forwards this pixel's gbuf_depth_ value
//      into gl_FragDepth so the pipeline's normal depth_test=true,
//      depth_write=false, LESS_OR_EQUAL compare against the shared (already
//      Early-Z-prepassed) depth attachment can still correctly reject
//      terrain pixels an NPC/prop already drew nearer -- this is the exact
//      same tolerant compare the forward terrain draw already relies on
//      against the prepass's deliberately-biased depth, just relocated to a
//      fullscreen pass instead of real per-vertex interpolated depth.
class TerrainShadingProjected {
public:
    bool Init(md::GpuDeviceHandle dev, int w, int h);
    void Shutdown();
    bool IsReady() const { return ready_; }

    // Recreates gbuf_color_/gbuf_depth_ at the new size if it differs from
    // what Init() (or the last EnsureSize call) allocated -- call once per
    // frame, before BeginGBufferPass, with the CURRENT window size. Unlike
    // the main swapchain (which SDL_GPU resizes automatically on present),
    // this class's own fixed-size render targets do NOT track window
    // resizes on their own -- a window resize/maximize that happens after
    // SceneRender::Init() (common: window managers often resize/reposition
    // a freshly-created window asynchronously, right after creation) left
    // this G-buffer permanently stuck at its stale init-time size, so the
    // terrain content it produces only covered part of the actual window
    // (background showing through the rest) -- diagnosed live 2026-08-02.
    void EnsureSize(md::GpuDeviceHandle dev, int w, int h);

    GpuTexture&      GBufferColor() { return gbuf_color_; }
    GpuDepthTexture&  GBufferDepth() { return gbuf_depth_; }

    // Begin the G-buffer geometry pass -- CLEARs both gbuf_color_ (unused
    // channels don't matter) and gbuf_depth_ (to 1.0). Caller draws terrain
    // instances into the returned pass via TerrainPatchRenderer::
    // DrawBatchGBuffer, then calls EndGBufferPass(). Must run BEFORE
    // DrawShadingResolve, outside (and before) the main color GpuRenderPass.
    SDL_GPURenderPass* BeginGBufferPass(md::GpuCommandBufferHandle cmd);
    void                EndGBufferPass();

    // Fullscreen resolve draw -- must run INSIDE the caller's already-open
    // main color render pass (same `rp`/`cmd` DrawBatch/DrawSlot would
    // otherwise be called with), in place of those calls. Parameter list
    // mirrors TerrainPatchRenderer::DrawBatch exactly (minus vp16/
    // patch_size/hmap, which this pass has no vertex geometry to need) so
    // callers can swap between the two variants with the same data.
    // shade_constant_debug: Крок 0 ablation (2026-08-23) -- true bypasses
    // TS_ComputeGroundAlbedo with a flat colour via world_params.w=1.0, a
    // single value for the whole draw (uniform branch, not per-pixel).
    // kenshi_blend_debug (task-terrain-kenshi-parity, 2026-09-26): true
    // sets world_params.w=2.0 instead, switching to the Kenshi-parity
    // blendMap-driven biome blend (TS_ComputeKenshiBiomeBlend,
    // terrain_kenshi_blend.glsl) for live A/B comparison against the
    // default zone-tent scheme -- see that file's own doc comment. The two
    // flags are mutually exclusive by construction (kenshi checked first
    // in the shader); passing both true selects kenshi.
    // БОРГ-TERRAIN-2 (2026-09-13): `vt` (TerrainVtPageCache) parameter
    // removed -- the cache-hit sampling path it fed (VT_SampleAlbedo in
    // terrain_shading_screenspace.frag) was never called from that
    // shader's main() (VT caching has been permanently disabled since
    // 2026-08-09), confirmed via grep before removal.
    void DrawShadingResolve(SDL_GPURenderPass* rp, md::GpuCommandBufferHandle cmd,
                             const TerrainRenderer::SunParams& sun,
                             float cam_x, float cam_y, float cam_z,
                             float world_origin_x, float world_origin_z, float world_to_uv,
                             float fog_far, const float fog_color[3], float fog_near,
                             const TerrainRenderer& ground,
                             bool shade_constant_debug = false,
                             bool kenshi_blend_debug = false);

private:
    bool CreateTextures(int w, int h);

    GpuTexture       gbuf_color_;
    GpuDepthTexture   gbuf_depth_;
    // RESOLVE_OPT 4-draw spatial split (Крок 4/6, docs/RESOLVE_OPT.md,
    // 2026-09-19) REVERTED 2026-09-21 (tests/scenarios/resolve_split_ab.lua):
    // the split cut per-pixel SHADER INSTRUCTIONS (1662->281 on the cheap
    // path, INTEL_DEBUG-verified) but was never checked against real
    // GPU-ms. It made things worse: without a hardware discard (stencil
    // rejected earlier as disproportionate blast radius), each of the 4
    // pipelines rasterized the FULL screen and fetched the G-buffer before
    // discarding non-matching pixels -- up to 4x the depth/G-buffer reads
    // per frame. Measured directly on P5 worst-zone (24,12), visually
    // confirmed via screenshot before trusting the numbers: single-draw
    // ~1.64ms vs the 4-draw split's ~6.8ms -- the split was ~4x SLOWER in
    // real wall-clock terms despite the real instruction-count win. Back
    // to one pipeline, one draw. A future spatial-split attempt MUST pair
    // with real hardware discard (stencil, or an indirect/compute tile-
    // classification dispatch) so the discard doesn't itself cost a full
    // G-buffer refetch.
    GpuPipeline       resolve_pipeline_;
    int  w_ = 0, h_ = 0;
    bool ready_ = false;
};
#endif

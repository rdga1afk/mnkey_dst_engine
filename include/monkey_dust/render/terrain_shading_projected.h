#pragma once
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
//   3. resolve_kenshi_pipeline_ -- fullscreen triangle (gl_VertexIndex, no vertex
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
    // #194: the textures of the previous size are kept (alt set); a return to that size swaps the sets without creating or destroying anything. Cached(w, h): EnsureSize(w, h) needs no GPU drain.
    // #194: the frame the size changed: the previous frame's depth is still the one of the old size (the reuse resolve reads it at its own size); null otherwise
    bool CrossSize() const { return xs_tex_ != nullptr; }
    bool Cached(int w, int h) const { return w == w_ && h == h_ ? true : (alt_color_.SDLTexture() && alt_depth_.SDLTexture() && alt_w_ == w && alt_h_ == h); }

    GpuTexture&      GBufferColor() { return gbuf_color_; }
    GpuDepthTexture&  GBufferDepth() { return *gd_cur_; }
    // #163 5b-2b: ground shading reuse needs the previous frame's G-buffer depth. History on: a second depth texture is made (EnsureHistory, outside a frame) and BeginFrame()
    // swaps the two once per rendered frame, before the G-buffer pass; GBufferDepth() is always this frame's, PrevGBufferDepth() the last one's. History off: one texture, no swap.
    void EnsureHistory(bool on, int w, int h);
    void BeginFrame() { if (hist_on_ && gd_alt_.SDLTexture()) { GpuDepthTexture* t = gd_cur_; gd_cur_ = gd_prev_; gd_prev_ = t; } }
    GpuDepthTexture&  PrevGBufferDepth() { return *gd_prev_; }
    bool HistoryReady() const { return hist_on_ && gd_alt_.SDLTexture() != nullptr; }
    // The reuse resolve variant (-DTS_REUSE): history colour (the previous frame's ground target) + the matrices of the reprojection; null = the plain variant.
    struct ReuseParams {
        SDL_GPUTexture* hist_colour = nullptr; SDL_GPUSampler* hist_sampler = nullptr;
        float p_old[16], p_new[16], t[16];   // column-major
        float params[4];                     // x = history valid, y = camera static, z = live block phase
    };
    void SetReuse(const ReuseParams* r) { reuse_ = r; }
    // #140 L2: the Kenshi main light variant: sun direction (to the sun), sun colour + strength, (envColour.w, ambientParams.w, exposure); null = the current lighting.
    struct KLightParams { float sun_dir[4], sun_colour[4], params[4], sun_real[4], horizon[4], fog[4]; };
    void SetKLight(const KLightParams* k) { klight_ = k; }
    // #140 L4: the ground target is RGBA16F and holds the colour before the exposure (klight variants with a float colour target)
    void SetFloatTarget(bool f) { float_target_ = f; }
    // #207 stencil split (measurement prototype, MD_STENCIL_SPLIT=1, default off): pass A = the lean reproject-only shader (accepted pixels write the colour and stencil 1, the live ones discard),
    // pass B = the heavy non-reuse shader with the stencil test NOT_EQUAL 1 (the hardware rejects the accepted pixels before the shader starts). Both in the ground pass, whose depth
    // attachment must then be a D32_FLOAT_S8_UINT texture (SetStencilSplitActive(true) by the caller that opened such a pass).
    bool StencilSplitReady() const { return split_ready_; }
    void SetStencilSplitActive(bool a) { split_active_ = a; }

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
    // The ground is always shaded by the Kenshi-parity variant (the zone-tent resolve and its A/B toggles were removed in #185 K4b).
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
                             const TerrainRenderer& ground);

    // S7 (docs/TERRAIN_SURFACE_PLAN_2026-10-02.md): the G-buffer pass can be depth-only
    // (no colour target); DrawShadingResolve then reconstructs world position from the
    // G-buffer depth (inv_vp) and the normal from the world normal texture. Set by
    // CullAndPrepass each frame BEFORE DrawShadingResolve. mode: 0 off (read gbuf_color_),
    // 1 reconstruct.
    struct S7State {
        float inv_vp[16] = {};
        float world_extent = 0.f, res_texels = 0.f;
        SDL_GPUTexture* normal_tex = nullptr;
        SDL_GPUSampler* normal_samp = nullptr;
        int mode = 0;
    };
    void SetS7(const S7State& s) { s7_ = s; }
    void SetSkipFlatOnCliff(bool on) { skip_flat_on_cliff_ = on; }

    bool        S7Reconstructing() const { return s7_.mode == 1; }
    const float* S7InvVp() const { return s7_.inv_vp; }

    // "terrain_units10" (docs/TERRAIN_SURFACE_PLAN_2026-10-02.md): Kenshi ground UV scale (x10 finer than the
    // metre-based 1/5000) + analytic live mip in the resolve. pix_scale = 2*tan(fovy/2); the shader divides by the
    // G-buffer height to get radians per pixel. Set each frame BEFORE DrawShadingResolve.
    void SetGroundUnits(bool on, float pix_scale) { units10_ = on; pix_scale_ = pix_scale; }

private:
    S7State s7_;
    bool  units10_ = false;
    float pix_scale_ = 0.f;
    bool CreateTextures(int w, int h);

    GpuTexture       gbuf_color_;
    GpuDepthTexture   gbuf_depth_;
    GpuDepthTexture   gd_alt_;                    // the other depth texture of the history ping-pong (made by EnsureHistory)
    GpuTexture        alt_color_;
    GpuDepthTexture   alt_depth_, alt_hist_;
    int alt_w_ = 0, alt_h_ = 0;
    SDL_GPUTexture* xs_tex_ = nullptr; SDL_GPUSampler* xs_samp_ = nullptr;
    GpuDepthTexture*  gd_cur_ = &gbuf_depth_;
    GpuDepthTexture*  gd_prev_ = &gd_alt_;
    bool hist_on_ = false;
    const ReuseParams* reuse_ = nullptr;
    const KLightParams* klight_ = nullptr;
    bool float_target_ = false;
    GpuPipeline resolve_klight_f_pipeline_, resolve_reuse_klight_f_pipeline_;
    GpuPipeline resolve_split_p_[2][3];   // #207 [RGBA8 / RGBA16F target][A lean reuse-only, B heavy with stencil test, C plain heavy for the frames without a history]
    bool split_ready_ = false, split_active_ = false;
    bool resolve_klight_f_ready_ = false, resolve_reuse_klight_f_ready_ = false;
    GpuPipeline resolve_kenshi_klight_pipeline_, resolve_kenshi_reuse_klight_pipeline_;
    bool resolve_klight_ready_ = false, resolve_reuse_klight_ready_ = false;
    GpuPipeline resolve_kenshi_reuse_pipeline_;
    bool resolve_kenshi_reuse_ready_ = false;
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
    // task #227: the resolve shader built with -DTS_KENSHI_FULL=1 (full-layer Kenshi-parity blend); the only ground resolve pipeline.
    GpuPipeline       resolve_kenshi_pipeline_;
    bool              resolve_kenshi_ready_ = false;
    // #155 step 1: the Kenshi variant that does not skip the flat layer chain where cliff_w >= 1 (pixel-identical output, A/B only)
    GpuPipeline       resolve_kenshi_noskip_pipeline_;
    bool              resolve_kenshi_noskip_ready_ = false;
    bool              skip_flat_on_cliff_ = true;
    struct ResolveCtx {
        const TerrainRenderer::SunParams* sun = nullptr;
        float cam_x = 0.f, cam_y = 0.f, cam_z = 0.f, world_origin_x = 0.f, world_origin_z = 0.f, world_to_uv = 0.f, fog_far = 0.f;
        const float* fog_color = nullptr;
        float fog_near = 0.f;
        const TerrainRenderer* ground = nullptr;
    };
    // The resource set the resolve binds (UBOs, 16 samplers, the probe SSBO) + the fullscreen triangle.
    void DrawFullscreen(SDL_GPURenderPass* rp, md::GpuCommandBufferHandle cmd, GpuPipeline& pipeline, const ResolveCtx& c);
    int  w_ = 0, h_ = 0;
    bool ready_ = false;
};

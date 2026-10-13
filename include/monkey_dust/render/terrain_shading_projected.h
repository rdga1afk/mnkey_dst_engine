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

    // ── #155 step 2: ground cache, ring 0 (docs/research/TERRAIN_CACHE_DESIGN.md) ───────────────────────────────────────────────────────────────
    // A 2048^2 ring of 4 cm texels centred on the camera holds what the Kenshi ground shader computes before lighting (albedo + tangent-space normal, 8 B
    // per texel in R16G16B16A16_UINT); the resolve reads it (bilinear by hand) inside the ring and runs the live shader outside. Fixed centre: the ring is
    // rebuilt (whole) when the camera leaves its middle, no toroidal scroll yet. Heights / normals come from the LOD's fine height window.
    // Size and density are runtime parameters (default 2048 / 4 cm); a change recreates the texture and invalidates the contents.
    // rings = 1..10: ring k has texel size texel_m * 2^k (reach N * texel / 2 each way), sits in rows kN..(k+1)N of the same texture and follows ring 0's centre.
    void  ConfigureRing(int n, float texel_m, int rings);
    int   RingN() const { return ring_n_; }
    int   RingCount() const { return ring_count_; }
    float RingTexelM() const { return ring_texel_m_; }
    struct FineWindow { SDL_GPUTexture* tex = nullptr; SDL_GPUSampler* samp = nullptr; SDL_GPUTexture* world_tex = nullptr; SDL_GPUSampler* world_samp = nullptr; float world_cell_m = 3.6f; float origin_x_m = 0.f, origin_z_m = 0.f, hmin = 0.f, hrange = 1.f; int cells = 0; float cell_m = 1.8f; };
    // Creates the ring texture on first use. Returns false when the bake pipeline or the texture is missing (the cache stays off).
    bool EnsureGroundRing();
    // Keeps the rings centred near (target_x, target_z) (metres). Toroidal: world texel w lives at physical (w & (N-1)), so moving the centre bakes only the
    // newly exposed column / row strips (scissored fullscreen draws, one render pass that keeps the rest). A ring that has just become active, the first build
    // and a jump of >= 3/4 of ring 0 refill in row bands of at most budget_texels per call (coarsest ring first; the resolve runs the live shader there until a ring
    // is complete); scrolling is limited to the same budget per call. force_full bakes all active rings now, centred exactly (measurement / static frames).
    // Which rings are active follows the camera's height above the ground (cam_h, metres; 0 = all rings): a ring is baked and read only when its texel is at least
    // select_factor times a screen pixel at the nearest ground point (cam_h * pixel angle; hysteresis: dropped below 2/3 of that). Own render pass: call outside any open pass, after the fine
    // window is current. Returns the number of texels baked this call.
    int UpdateGroundRing(md::GpuCommandBufferHandle cmd, float cam_x, float cam_y, float cam_z, float world_origin_x, float world_origin_z,
                         float world_to_uv, const TerrainRenderer& ground, const FineWindow& fw, float target_x, float target_z, int budget_texels, bool force_full,
                         float cam_h, float select_factor);
    int   RingMask() const { return ring_mask_; }
    float RingMaxReachM() const { return 0.5f * (float)ring_n_ * ring_texel_m_ * (float)(1 << (ring_count_ - 1)); }
    void  SetRingUse(bool on) { ring_use_ = on; }
    void  SetRingVariantForced(bool on) { ring_variant_forced_ = on; } // measurement: the ring shader variant with the cache off
    bool  RingValid() const { return ring_valid_; }
    void  InvalidateRing() { ring_valid_ = false; }
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
    // task #227: same resolve shader built with -DTS_KENSHI_FULL=1 (full-layer Kenshi-parity blend);
    // used only while the DEBUG kenshi_blend toggle is on, so the default pipeline is untouched.
    GpuPipeline       resolve_kenshi_pipeline_;
    bool              resolve_kenshi_ready_ = false;
    // #155 step 1: the Kenshi variant that does not skip the flat layer chain where cliff_w >= 1 (pixel-identical output, A/B only)
    GpuPipeline       resolve_kenshi_noskip_pipeline_;
    bool              resolve_kenshi_noskip_ready_ = false;
    // #155 step 2b: the Kenshi resolve WITH the ground cache read (+233 instructions, SIMD8): used only while the cache is on (or forced); the default Kenshi pipeline has no ring code
    GpuPipeline       resolve_kenshi_ring_pipeline_;
    bool              resolve_kenshi_ring_ready_ = false;
    bool              ring_variant_forced_ = false;
    bool              skip_flat_on_cliff_ = true;
    // #155 step 2 ground cache ring
    GpuPipeline       bake_pipeline_;
    bool              bake_ready_ = false;
    GpuTexture        ring_color_;
    int               ring_n_ = 2048;
    int               ring_count_ = 1;
    static constexpr int kMaxRings = 10;
    int               ring_fill_row_[kMaxRings] = {};
    int               ring_ci_k_[kMaxRings][2] = {};   // centre texel of ring k (in its own texels)
    bool              ring_active_[kMaxRings] = {};    // ring k is baked / kept up to date (chosen by the camera's height)
    int               ring_mask_ = 0;                  // bit k: ring k active and completely baked (what the resolve may read)
    float             ring_texel_m_ = 0.04f;
    bool              ring_ready_ = false;
    bool              ring_valid_ = false;
    bool              ring_use_ = false;
    float             ring_cx_ = 0.f, ring_cz_ = 0.f; // window centre, metres (= ring_ci_ * texel)
    int               ring_ci_x_ = 0, ring_ci_z_ = 0;  // window centre, world texel index
    bool              ring_have_ = false;
    struct ResolveCtx {
        const TerrainRenderer::SunParams* sun = nullptr;
        float cam_x = 0.f, cam_y = 0.f, cam_z = 0.f, world_origin_x = 0.f, world_origin_z = 0.f, world_to_uv = 0.f, fog_far = 0.f;
        const float* fog_color = nullptr;
        float fog_near = 0.f;
        const TerrainRenderer* ground = nullptr;
        bool shade_constant_debug = false, kenshi_blend_debug = false;
    };
    // The resource set both the resolve and the bake draw bind (UBOs, 16 samplers, the probe SSBO) + the fullscreen triangle. bake: bindings 7/8 = the fine
    // window, UBO inv_vp / recon carry the ring (bake_cols: 12 floats = three vec4 columns).
    void DrawFullscreen(SDL_GPURenderPass* rp, md::GpuCommandBufferHandle cmd, GpuPipeline& pipeline, const ResolveCtx& c, bool bake,
                        const FineWindow* fw, const float* bake_cols);    int  w_ = 0, h_ = 0;
    bool ready_ = false;
};
#endif

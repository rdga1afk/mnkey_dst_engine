#include <monkey_dust/render/terrain_quadtree_renderer.h>
#include <monkey_dust/render/gpu_copy_pass.h>
#include <monkey_dust/render/gpu_pass_view.h>
#include <monkey_dust/render/gpu_hal_free_functions.h>
#ifdef MD_SDL_GPU
#include <monkey_dust/platform/md_log.h>
#include <monkey_dust/world/terrain_quadtree_mesh.h>
#include <cstring>

namespace {
// Field order matches shaders/terrain_quadtree.vert's TerrainQuadtreeUBO
// exactly (std140). morph/skirt fields are unused (always 0) since
// 2026-09-05 (docs/TERRAIN_FLAT_LOD_PLAN.md, fixed-depth tiling) but kept
// in the UBO layout -- the shader itself wasn't touched (see that doc:
// leaving morph=0/skirt_depth=0 as harmless no-ops in the existing shader
// was lower-risk than also editing shared vertex-shader math this pass).
struct TerrainQuadtreeUBO {
    float vp[16];
    float origin_size_texel_morph[4];
    float height_range[4];
    float cam_pos_skirt[4];
};
static_assert(sizeof(TerrainQuadtreeUBO) == 64 + 16 * 3, "TerrainQuadtreeUBO size mismatch");

// Field order matches shaders/terrain_quadtree_forward.frag's PatchFrag/
// ForwardCam exactly (std140) -- same layout as terrain_shading_projected.
// cpp's ProjFragUBO/ProjCamUBO, duplicated here rather than shared since
// the two classes have no common base and this is the only member that
// would be shared.
struct ForwardFragUBO {
    float sun_dir_str[4];
    float ambient[4];
    float world_params[4];
    float fog_color_near[4];
    float fog_far;
    float _pad[3];
};
static_assert(sizeof(ForwardFragUBO) == 80, "ForwardFragUBO size mismatch");

struct ForwardCamUBO {
    float cam_pos_ws[4];
};
static_assert(sizeof(ForwardCamUBO) == 16, "ForwardCamUBO size mismatch");
} // namespace

bool TerrainQuadtreeRenderer::Init(md::GpuDeviceHandle /*dev*/) {
    md::TerrainQuadtreeMesh mesh = md::BuildTerrainQuadtreeMesh();

    filled_ibo_.Init(0x8893u /*GL_ELEMENT_ARRAY_BUFFER*/, mesh.filled_indices.data(),
                      mesh.filled_indices.size() * sizeof(uint32_t));
    filled_index_count_ = (uint32_t)mesh.filled_indices.size();

    GpuPipeline::Desc pd;
    pd.layout.count       = 0; // vertex-buffer-less -- gl_VertexIndex comes from the bound IBO alone
    pd.raster.depth_test  = true;
    pd.raster.depth_write = true;
    pd.raster.cull_back   = false; // skirt quads face outward on all 4 borders
    pd.has_depth_target   = true;
    pd.vert_uniform_bufs  = 1;
    pd.vert_samplers      = 2; // 2026-08-24: #398 reverted -- heightTex + normalTex (world-wide)
    pd.vert_path = "shaders/terrain_quadtree.vert";
    pd.frag_path = "shaders/terrain_gbuffer_mini.frag"; // RESOLVE_OPT spatial-split Крок 4: now computes boundary-mask bit
    pd.frag_uniform_bufs = 0;
    pd.frag_samplers     = 3; // Крок 4: tex_ground, tex_ground_nml, zoneGroundLayersTex (TS_NeedsCornerBlend)
    pd.frag_storage_bufs = 0;
    pd.color_format = SDL_GPU_TEXTUREFORMAT_R32G32B32A32_FLOAT;
    if (!gbuffer_pipeline_.Create(pd)) {
        MD_LOG(MD_LOG_WARNING, "[TerrainQuadtreeRenderer] pipeline create failed");
        return false;
    }
    ready_ = true;
    return true;
}

bool TerrainQuadtreeRenderer::InitForward(md::GpuDeviceHandle /*dev*/) {
    GpuPipeline::Desc pd;
    pd.layout.count       = 0; // vertex-buffer-less, same IBO-only technique as gbuffer_pipeline_
    pd.raster.depth_test  = true;
    pd.raster.depth_write = true;
    pd.raster.cull_back   = false; // skirt quads face outward on all 4 borders
    pd.has_depth_target   = true;
    pd.vert_uniform_bufs  = 1;
    pd.vert_samplers      = 2; // heightTex + normalTex (world-wide), same as gbuffer_pipeline_
    pd.vert_path = "shaders/terrain_quadtree.vert"; // shared, unmodified -- see that file's own doc comment
    pd.frag_path = "shaders/terrain_quadtree_forward.frag";
    pd.frag_uniform_bufs = 2; // set=3 binding=0 PatchFrag, binding=1 ForwardCam
    pd.frag_samplers     = 9; // set=2: tex_colour,tex_ground,tex_ground_baked,tex_overlay_mask,tex_ground_nml(task #12),tex_detail_array,tex_detail_tint(КРОК3),zoneGroundLayersTex,texSteepnessSmoothed(2026-09-24)
    pd.frag_storage_bufs = 0;
    // color_format left INVALID -- draws into the caller's real swapchain-
    // format main color target, not an isolated G-buffer (the whole point
    // of reviving forward shading: no separate G-buffer texture at all).
    if (!forward_pipeline_.Create(pd)) {
        MD_LOG(MD_LOG_WARNING, "[TerrainQuadtreeRenderer] forward pipeline create failed");
        return false;
    }
    forward_ready_ = true;
    return true;
}

bool TerrainQuadtreeRenderer::InitBatched(md::GpuDeviceHandle dev) {
    if (!dev) return false;

    SDL_GPUTextureCreateInfo ti{};
    ti.type                 = SDL_GPU_TEXTURETYPE_2D;
    ti.width                = (Uint32)kNodeDataTexWidth;
    ti.height               = (Uint32)((2 * kMaxBatchedNodes + kNodeDataTexWidth - 1) / kNodeDataTexWidth);
    ti.layer_count_or_depth = 1;
    ti.num_levels           = 1;
    ti.format               = SDL_GPU_TEXTUREFORMAT_R32G32B32A32_FLOAT;
    ti.usage                = SDL_GPU_TEXTUREUSAGE_SAMPLER;
    node_data_tex_ = GpuCreateTexture(dev, &ti);
    if (!node_data_tex_) {
        MD_LOG(MD_LOG_WARNING, "[TerrainQuadtreeRenderer] node_data_tex create failed: %s", SDL_GetError());
        return false;
    }

    SDL_GPUSamplerCreateInfo si{};
    si.min_filter     = SDL_GPU_FILTER_NEAREST;
    si.mag_filter     = SDL_GPU_FILTER_NEAREST;
    si.mipmap_mode    = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
    si.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    si.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    si.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    node_data_sampler_ = GpuCreateSampler(dev, &si);
    if (!node_data_sampler_) {
        MD_LOG(MD_LOG_WARNING, "[TerrainQuadtreeRenderer] node_data_sampler create failed");
        return false;
    }

    GpuPipeline::Desc pd;
    pd.layout.count       = 0; // vertex-buffer-less, same IBO-only technique as gbuffer_pipeline_
    pd.raster.depth_test  = true;
    pd.raster.depth_write = true;
    pd.raster.cull_back   = false;
    pd.has_depth_target   = true;
    pd.vert_uniform_bufs  = 1;
    pd.vert_samplers      = 3; // heightTex, normalTex, nodeDataTex -- see .vert's own doc comment
    pd.vert_path = "shaders/terrain_quadtree_batched.vert";
    pd.frag_path = "shaders/terrain_gbuffer_mini.frag"; // RESOLVE_OPT spatial-split Крок 4: now computes boundary-mask bit, same as gbuffer_pipeline_
    pd.frag_uniform_bufs = 0;
    // Крок 4 (2026-09-19): was 0, "MUST stay 0 -- see .vert's doc comment
    // on why" -- that comment's actual concern (vert_storage_bufs>0 +
    // frag_samplers>0) doesn't apply here (.vert uses nodeDataTex, a
    // TEXTURE not an SSBO, vert_storage_bufs=0). The OTHER historical
    // concern (vert_samplers>0 + frag_samplers>0, a suspected Gen9 hang)
    // was downgraded to an INFO log in gpu_hal_pipeline.cpp 2026-07-25
    // after an isolated test disproved it on this exact hardware/driver
    // stack -- verified against that guard's current code before making
    // this change, not assumed. See .vert's own doc comment, also updated.
    // Крок 4 (2026-09-19): was 0, "MUST stay 0 -- see .vert's doc comment
    // on why" -- that comment's actual concern (vert_storage_bufs>0 +
    // frag_samplers>0) doesn't apply here (.vert uses nodeDataTex, a
    // TEXTURE not an SSBO, vert_storage_bufs=0). The OTHER historical
    // concern (vert_samplers>0 + frag_samplers>0, a suspected Gen9 hang)
    // was downgraded to an INFO log in gpu_hal_pipeline.cpp 2026-07-25
    // after an isolated test disproved it on this exact hardware/driver
    // stack. Root cause of the real Крок 4 regression turned out to be
    // an unrelated debug-shader artifact, NOT this combination -- see
    // docs/RESOLVE_OPT.md's Крок 4 investigation log.
    pd.frag_samplers     = 3; // tex_ground, tex_ground_nml, zoneGroundLayersTex (TS_NeedsCornerBlend)
    pd.frag_storage_bufs = 0;
    pd.color_format = SDL_GPU_TEXTUREFORMAT_R32G32B32A32_FLOAT;
    if (!batched_pipeline_.Create(pd)) {
        MD_LOG(MD_LOG_WARNING, "[TerrainQuadtreeRenderer] batched pipeline create failed");
        return false;
    }
    batched_ready_ = true;
    return true;
}

void TerrainQuadtreeRenderer::UploadNodeData(md::GpuDeviceHandle dev, SDL_GPUCopyPass* cp,
                                              const TerrainQuadtree::VisibleNode* nodes, int count) {
    if (!batched_ready_ || !dev || !cp || count <= 0) return;
    if (count > kMaxBatchedNodes) count = kMaxBatchedNodes;

    constexpr float kPatchQuads = 16.0f;
    // static: this is per-frame scratch, far too large for a stack frame
    // (kMaxBatchedNodes*2 float4 = 256KB) -- same convention as the
    // static VisibleNode arrays at every SelectVisible call site.
    static float staging[kMaxBatchedNodes * 2 * 4];
    for (int i = 0; i < count; ++i) {
        float texelSize = nodes[i].size / kPatchQuads;
        float* a = staging + (size_t)i * 8;
        float* b = a + 4;
        a[0] = nodes[i].origin_x;
        a[1] = nodes[i].origin_z;
        a[2] = texelSize;
        a[3] = 0.f; // morph, unused (see TerrainQuadtreeUBO doc comment)
        b[0] = 0.f; // skirt_depth, unused
        b[1] = 0.f; b[2] = 0.f; b[3] = 0.f;
    }

    // Node texels are laid out contiguously starting at texel 0: texels
    // 0..2*count-1 span rows [0, (2*count-1)/width]. The transfer buffer
    // must cover the FULL rectangular region SDL_GPU is told to upload
    // (kNodeDataTexWidth*rows texels), not just the real `count` payload,
    // or SDL_GPU reads past the buffer -- so size it to the region and
    // zero-pad the tail (harmless: DrawBatched's instance_count caps
    // gl_InstanceIndex at `count`, so padding texels are never sampled).
    int texel_count  = count * 2;
    int rows         = (texel_count + kNodeDataTexWidth - 1) / kNodeDataTexWidth;
    Uint32 upload_bytes = (Uint32)((size_t)count * 8 * sizeof(float));
    Uint32 region_bytes = (Uint32)kNodeDataTexWidth * (Uint32)rows * 4 * sizeof(float);

    SDL_GPUTransferBufferCreateInfo tbi{};
    tbi.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    tbi.size  = region_bytes;
    SDL_GPUTransferBuffer* tb = GpuCreateTransferBuffer(dev, &tbi);
    if (!tb) return;
    void* map = GpuMapTransfer(tb, false);
    if (map) {
        if (region_bytes > upload_bytes) memset(map, 0, region_bytes);
        memcpy(map, staging, upload_bytes);
    }
    GpuUnmapTransfer(tb);

    SDL_GPUTextureTransferInfo src{};
    src.transfer_buffer = tb;
    src.pixels_per_row   = (Uint32)kNodeDataTexWidth;
    src.rows_per_layer   = (Uint32)rows;
    SDL_GPUTextureRegion dst{};
    dst.texture = node_data_tex_;
    dst.w = (Uint32)kNodeDataTexWidth; dst.h = (Uint32)rows; dst.d = 1;
    // FromRaw: cp is a pass the CALLER opened (npc_render_frame_prep.cpp) and
    // will End() itself -- no cmd needed here, no upload/download method uses it.
    GpuCopyPass::FromRaw(cp, nullptr).UploadTexture(src, dst, false);
    GpuReleaseTransferBuffer(dev, tb);
}

void TerrainQuadtreeRenderer::BeginBatched(SDL_GPURenderPass* rp, md::GpuCommandBufferHandle cmd,
                                            const TerrainWorldHeightmap& hmap, const float* vp16,
                                            float cam_x, float cam_y, float cam_z,
                                            const TerrainRenderer& ground) {
    if (!batched_ready_) return;
    GpuPassView pv = GpuPassView::FromRaw(rp, cmd);
    pv.BindPipeline(&batched_pipeline_);

    struct TerrainBatchUBO {
        float vp[16];
        float height_range[4];
        float cam_pos_pad[4];
    } ubo{};
    std::memcpy(ubo.vp, vp16, 64);
    ubo.height_range[0] = hmap.HeightMin();
    ubo.height_range[1] = hmap.HeightMax();
    ubo.height_range[2] = hmap.WorldExtent();
    ubo.height_range[3] = (float)hmap.Resolution();
    ubo.cam_pos_pad[0] = cam_x;
    ubo.cam_pos_pad[1] = cam_y;
    ubo.cam_pos_pad[2] = cam_z;
    ubo.cam_pos_pad[3] = 0.f;
    pv.PushVertexUniforms(0, &ubo, sizeof(ubo));

    SDL_GPUTextureSamplerBinding samp[3] = {
        { hmap.Texture(), hmap.Sampler() },
        { hmap.NormalTexture(), hmap.NormalSampler() },
        { node_data_tex_, node_data_sampler_ },
    };
    pv.BindVertexSamplers(0, samp, 3);

    // RESOLVE_OPT spatial-split plan, Крок 4 -- see DrawNode's own doc
    // comment on this exact binding block; DrawBatched itself binds
    // nothing per-call, so this is the only place to bind it for the
    // batched path.
    SDL_GPUTextureSamplerBinding ground_all[7];
    ground.GetSharedGroundSamplers(ground_all);
    // Slots 1/4 (tex_ground_array/tex_ground_nml_array) have no fallback
    // of a matching image type in FillSamplerBindings and fall back to
    // nullptr/nullptr if the real asset failed to load -- same null risk
    // DrawShadingResolve already guards against.
    if (!ground_all[1].texture || !ground_all[1].sampler
        || !ground_all[4].texture || !ground_all[4].sampler) {
        return;
    }
    SDL_GPUTextureSamplerBinding frag_samp[3] = {
        ground_all[1], // tex_ground
        ground_all[4], // tex_ground_nml
        { ground.ZoneGroundLayersTexture(), ground.ZoneGroundLayersSampler() },
    };
    pv.BindFragmentSamplers(0, frag_samp, 3);
}

void TerrainQuadtreeRenderer::DrawBatched(SDL_GPURenderPass* rp, md::GpuCommandBufferHandle cmd, int count) {
    if (!batched_ready_ || count <= 0) return;
    if (count > kMaxBatchedNodes) count = kMaxBatchedNodes;
    GpuPassView pv = GpuPassView::FromRaw(rp, cmd);

    pv.BindIndexBuffer(&filled_ibo_, SDL_GPU_INDEXELEMENTSIZE_32BIT);
    pv.DrawIndexed(filled_index_count_, (uint32_t)count, 0, 0, 0);
}

void TerrainQuadtreeRenderer::Shutdown(md::GpuDeviceHandle dev) {
    gbuffer_pipeline_.Destroy();
    if (forward_ready_) forward_pipeline_.Destroy();
    if (wireframe_ready_) wireframe_pipeline_.Destroy();
    if (batched_wireframe_ready_) batched_wireframe_pipeline_.Destroy();
    if (batched_ready_) {
        batched_pipeline_.Destroy();
        if (dev && node_data_tex_) GpuReleaseTexture(dev, node_data_tex_);
        if (dev && node_data_sampler_) GpuReleaseSampler(dev, node_data_sampler_);
        node_data_tex_ = nullptr;
        node_data_sampler_ = nullptr;
    }
    filled_ibo_.Shutdown();
    ready_ = false;
    forward_ready_ = false;
    wireframe_ready_ = false;
    batched_ready_ = false;
    batched_wireframe_ready_ = false;
}

void TerrainQuadtreeRenderer::DrawNode(SDL_GPURenderPass* rp, md::GpuCommandBufferHandle cmd,
                                        const TerrainWorldHeightmap& hmap, const float* vp16,
                                        const TerrainQuadtree::VisibleNode& node,
                                        float cam_x, float cam_y, float cam_z,
                                        const TerrainRenderer& ground) {
    if (!ready_) return;

    // texelSize = this node's own world footprint / 16 quads (kPatchQuads,
    // terrain_quadtree_mesh.h) -- matches the shader's own kGridSize=17 decode.
    constexpr float kPatchQuads = 16.0f;
    float texelSize = node.size / kPatchQuads;

    GpuPassView pv = GpuPassView::FromRaw(rp, cmd);
    pv.BindPipeline(&gbuffer_pipeline_);

    TerrainQuadtreeUBO ubo{};
    std::memcpy(ubo.vp, vp16, 64);
    ubo.origin_size_texel_morph[0] = node.origin_x;
    ubo.origin_size_texel_morph[1] = node.origin_z;
    ubo.origin_size_texel_morph[2] = texelSize;
    ubo.origin_size_texel_morph[3] = 0.f; // morph, unused (see UBO struct doc comment)
    ubo.height_range[0] = hmap.HeightMin();
    ubo.height_range[1] = hmap.HeightMax();
    ubo.height_range[2] = hmap.WorldExtent();
    ubo.height_range[3] = (float)hmap.Resolution();
    ubo.cam_pos_skirt[0] = cam_x;
    ubo.cam_pos_skirt[1] = cam_y;
    ubo.cam_pos_skirt[2] = cam_z;
    ubo.cam_pos_skirt[3] = 0.f; // skirt_depth, unused
    pv.PushVertexUniforms(0, &ubo, sizeof(ubo));

    SDL_GPUTextureSamplerBinding samp[2] = {
        { hmap.Texture(), hmap.Sampler() },
        { hmap.NormalTexture(), hmap.NormalSampler() },
    };
    pv.BindVertexSamplers(0, samp, 2);

    // RESOLVE_OPT spatial-split plan, Крок 4: terrain_gbuffer_mini.frag's
    // boundary-mask computation (TS_NeedsCornerBlend) needs tex_ground(1)/
    // tex_ground_nml(4) from GetSharedGroundSamplers' 7-slot layout, plus
    // ZoneGroundLayersTexture -- bound at set=2 binding=0/1/2 to match
    // that shader's declaration order.
    SDL_GPUTextureSamplerBinding ground_all[7];
    ground.GetSharedGroundSamplers(ground_all);
    if (!ground_all[1].texture || !ground_all[1].sampler
        || !ground_all[4].texture || !ground_all[4].sampler) {
        return;
    }
    SDL_GPUTextureSamplerBinding frag_samp[3] = {
        ground_all[1], // tex_ground
        ground_all[4], // tex_ground_nml
        { ground.ZoneGroundLayersTexture(), ground.ZoneGroundLayersSampler() },
    };
    pv.BindFragmentSamplers(0, frag_samp, 3);

    pv.BindIndexBuffer(&filled_ibo_, SDL_GPU_INDEXELEMENTSIZE_32BIT);
    pv.DrawIndexed(filled_index_count_, 1, 0, 0, 0);
}

void TerrainQuadtreeRenderer::BeginForward(SDL_GPURenderPass* rp, md::GpuCommandBufferHandle cmd,
                                            const TerrainRenderer::SunParams& sun,
                                            float cam_x, float cam_y, float cam_z,
                                            float world_origin_x, float world_origin_z, float world_to_uv,
                                            float fog_far, const float fog_color[3], float fog_near,
                                            const TerrainRenderer& ground) {
    if (!forward_ready_) return;
    GpuPassView pv = GpuPassView::FromRaw(rp, cmd);
    pv.BindPipeline(&forward_pipeline_);

    ForwardFragUBO fubo{};
    fubo.sun_dir_str[0] = sun.dir[0]; fubo.sun_dir_str[1] = sun.dir[1];
    fubo.sun_dir_str[2] = sun.dir[2]; fubo.sun_dir_str[3] = sun.strength;
    fubo.ambient[0]     = sun.ambient[0]; fubo.ambient[1] = sun.ambient[1];
    fubo.ambient[2]     = sun.ambient[2]; fubo.ambient[3] = 0.f;
    fubo.world_params[0] = world_origin_x; fubo.world_params[1] = world_origin_z;
    fubo.world_params[2] = world_to_uv;    fubo.world_params[3] = 0.f;
    fubo.fog_color_near[0] = fog_color[0]; fubo.fog_color_near[1] = fog_color[1];
    fubo.fog_color_near[2] = fog_color[2]; fubo.fog_color_near[3] = fog_near;
    fubo.fog_far = fog_far;
    pv.PushFragmentUniforms(0, &fubo, sizeof(fubo));

    ForwardCamUBO cubo{};
    cubo.cam_pos_ws[0] = cam_x; cubo.cam_pos_ws[1] = cam_y;
    cubo.cam_pos_ws[2] = cam_z; cubo.cam_pos_ws[3] = 0.f;
    pv.PushFragmentUniforms(1, &cubo, sizeof(cubo));

    // set=2: same 7 shared ground samplers (task #12: +tex_ground_nml;
    // КРОК 3 2026-09-17: +tex_detail_array/+tex_detail_tint, required
    // structurally even on this dormant path -- terrain_shading_common.
    // glsl's TS_ComputeGroundAlbedo/TS_SampleZoneFlatDetail is ONE shared
    // GLSL source textually included here too, so its sampler
    // declarations must resolve in every including file regardless of
    // whether that file's draw path is reachable at runtime) +
    // zoneGroundLayersTex the resolve path binds -- contiguous 0..7, no VT
    // bindings needed (VT sampling is dead code on the live
    // ShadeTerrainGround path, see terrain_quadtree_forward.frag's own doc
    // comment). This whole draw path (Variant B, forward/inline shading) is
    // itself dormant -- use_forward_terrain_shading_ defaults false -- but
    // kept binding-correct in case it's ever revisited.
    SDL_GPUTextureSamplerBinding ground_bindings[7];
    ground.GetSharedGroundSamplers(ground_bindings);
    for (int i = 0; i < 7; ++i) {
        if (!ground_bindings[i].texture || !ground_bindings[i].sampler) return;
    }
    pv.BindFragmentSamplers(0, ground_bindings, 7);
    SDL_GPUTextureSamplerBinding zone_binding[1] = {
        { ground.ZoneGroundLayersTexture(), ground.ZoneGroundLayersSampler() },
    };
    pv.BindFragmentSamplers(7, zone_binding, 1);
    // bake/live cliff_w single-source-of-truth (2026-09-24) -- same
    // structural-resolution requirement as this function's own doc comment
    // above (dormant path, still binding-correct).
    SDL_GPUTextureSamplerBinding steepness_binding[1] = {
        { ground.SteepnessSmoothedTexture(), ground.SteepnessSmoothedSampler() },
    };
    pv.BindFragmentSamplers(8, steepness_binding, 1);
}

void TerrainQuadtreeRenderer::DrawNodeForward(SDL_GPURenderPass* rp, md::GpuCommandBufferHandle cmd,
                                               const TerrainWorldHeightmap& hmap, const float* vp16,
                                               const TerrainQuadtree::VisibleNode& node,
                                               float cam_x, float cam_y, float cam_z) {
    if (!forward_ready_) return;

    constexpr float kPatchQuads = 16.0f;
    float texelSize = node.size / kPatchQuads;

    TerrainQuadtreeUBO ubo{};
    std::memcpy(ubo.vp, vp16, 64);
    ubo.origin_size_texel_morph[0] = node.origin_x;
    ubo.origin_size_texel_morph[1] = node.origin_z;
    ubo.origin_size_texel_morph[2] = texelSize;
    ubo.origin_size_texel_morph[3] = 0.f; // morph, unused
    ubo.height_range[0] = hmap.HeightMin();
    ubo.height_range[1] = hmap.HeightMax();
    ubo.height_range[2] = hmap.WorldExtent();
    ubo.height_range[3] = (float)hmap.Resolution();
    ubo.cam_pos_skirt[0] = cam_x;
    ubo.cam_pos_skirt[1] = cam_y;
    ubo.cam_pos_skirt[2] = cam_z;
    ubo.cam_pos_skirt[3] = 0.f; // skirt_depth, unused
    GpuPassView pv = GpuPassView::FromRaw(rp, cmd);
    pv.PushVertexUniforms(0, &ubo, sizeof(ubo));

    SDL_GPUTextureSamplerBinding samp[2] = {
        { hmap.Texture(), hmap.Sampler() },
        { hmap.NormalTexture(), hmap.NormalSampler() },
    };
    pv.BindVertexSamplers(0, samp, 2);

    pv.BindIndexBuffer(&filled_ibo_, SDL_GPU_INDEXELEMENTSIZE_32BIT);
    pv.DrawIndexed(filled_index_count_, 1, 0, 0, 0);
}

bool TerrainQuadtreeRenderer::InitWireframe(md::GpuDeviceHandle /*dev*/) {
    GpuPipeline::Desc pd;
    pd.layout.count       = 0; // vertex-buffer-less, same IBO-only technique as gbuffer_pipeline_
    pd.raster.depth_test  = true;
    pd.raster.depth_write = false; // overlay only -- never corrupts the real depth buffer
    pd.raster.cull_back   = false; // see edges from both sides
    pd.raster.wireframe   = true;  // SDL_GPU_FILLMODE_LINE
    pd.has_depth_target   = true;
    pd.vert_uniform_bufs  = 1;
    pd.vert_samplers      = 2; // heightTex + normalTex (world-wide), same as gbuffer_pipeline_
    pd.vert_path = "shaders/terrain_quadtree.vert"; // shared, unmodified -- real geometry transform
    pd.frag_path = "shaders/terrain_wireframe.frag";
    pd.frag_uniform_bufs = 0;
    pd.frag_samplers     = 0;
    pd.frag_storage_bufs = 0;
    // color_format left INVALID -- draws into the caller's real swapchain-
    // format main colour target, same contract as forward_pipeline_.
    if (!wireframe_pipeline_.Create(pd)) {
        MD_LOG(MD_LOG_WARNING, "[TerrainQuadtreeRenderer] wireframe pipeline create failed");
        return false;
    }
    wireframe_ready_ = true;
    return true;
}

void TerrainQuadtreeRenderer::DrawNodeWireframe(SDL_GPURenderPass* rp, md::GpuCommandBufferHandle cmd,
                                                 const TerrainWorldHeightmap& hmap, const float* vp16,
                                                 const TerrainQuadtree::VisibleNode& node,
                                                 float cam_x, float cam_y, float cam_z) {
    if (!wireframe_ready_) return;

    constexpr float kPatchQuads = 16.0f;
    float texelSize = node.size / kPatchQuads;

    GpuPassView pv = GpuPassView::FromRaw(rp, cmd);
    pv.BindPipeline(&wireframe_pipeline_);

    TerrainQuadtreeUBO ubo{};
    std::memcpy(ubo.vp, vp16, 64);
    ubo.origin_size_texel_morph[0] = node.origin_x;
    ubo.origin_size_texel_morph[1] = node.origin_z;
    ubo.origin_size_texel_morph[2] = texelSize;
    ubo.origin_size_texel_morph[3] = 0.f; // morph, unused
    ubo.height_range[0] = hmap.HeightMin();
    ubo.height_range[1] = hmap.HeightMax();
    ubo.height_range[2] = hmap.WorldExtent();
    ubo.height_range[3] = (float)hmap.Resolution();
    ubo.cam_pos_skirt[0] = cam_x;
    ubo.cam_pos_skirt[1] = cam_y;
    ubo.cam_pos_skirt[2] = cam_z;
    ubo.cam_pos_skirt[3] = 0.f; // skirt_depth, unused
    pv.PushVertexUniforms(0, &ubo, sizeof(ubo));

    SDL_GPUTextureSamplerBinding samp[2] = {
        { hmap.Texture(), hmap.Sampler() },
        { hmap.NormalTexture(), hmap.NormalSampler() },
    };
    pv.BindVertexSamplers(0, samp, 2);

    pv.BindIndexBuffer(&filled_ibo_, SDL_GPU_INDEXELEMENTSIZE_32BIT);
    pv.DrawIndexed(filled_index_count_, 1, 0, 0, 0);
}

bool TerrainQuadtreeRenderer::InitBatchedWireframe(md::GpuDeviceHandle dev) {
    if (!dev || !batched_ready_) return false; // reuses InitBatched's node_data_tex_/sampler
    GpuPipeline::Desc pd;
    pd.layout.count       = 0; // vertex-buffer-less, same technique as batched_pipeline_
    pd.raster.topology    = GpuTopology::LINES; // real LINELIST, not FILLMODE_LINE over triangles
    pd.raster.depth_test  = true;
    // depth_write=true (unlike wireframe_pipeline_'s debug-overlay use):
    // this pipeline is PRIMARY content here (no shaded pass runs first to
    // populate depth), so tiles must write real depth or farther tiles can
    // overdraw nearer ones regardless of draw order.
    pd.raster.depth_write = true;
    pd.raster.cull_back   = false;
    pd.has_depth_target   = true;
    pd.vert_uniform_bufs  = 1;
    pd.vert_samplers      = 3; // heightTex, normalTex, nodeDataTex -- same as batched_pipeline_
    pd.vert_path = "shaders/terrain_quadtree_boundary.vert"; // tile outline only, NOT the internal grid
    pd.frag_path = "shaders/terrain_wireframe.frag";
    pd.frag_uniform_bufs = 0;
    pd.frag_samplers     = 0;
    pd.frag_storage_bufs = 0;
    // color_format explicit (UNLIKE wireframe_pipeline_'s "leave INVALID,
    // falls back to SDL_GetGPUSwapchainTextureFormat" -- correct for that
    // pipeline's game/editor MAIN swapchain-backed pass, real live-verified
    // there). This method is editor-only (no game call site) and the
    // editor's 3D World tab draws into an OFF-SCREEN RTT (s_color,
    // editor_world_3d_sdlgpu.cpp's ensure_rtt), NOT the swapchain -- the
    // INVALID fallback silently created a pipeline incompatible with that
    // render pass (VUID-vkCmdDraw-renderPass-02684, confirmed via
    // VK_LAYER_KHRONOS_validation), which the driver tolerated for cheap
    // draws but hard-hung (VK_ERROR_DEVICE_LOST) once this pipeline's much
    // heavier batched-instanced LINE-mode draw ran. Must match ensure_rtt's
    // ci.format exactly.
    pd.color_format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    if (!batched_wireframe_pipeline_.Create(pd)) {
        MD_LOG(MD_LOG_WARNING, "[TerrainQuadtreeRenderer] batched wireframe pipeline create failed");
        return false;
    }
    batched_wireframe_ready_ = true;
    return true;
}

void TerrainQuadtreeRenderer::BeginBatchedWireframe(SDL_GPURenderPass* rp, md::GpuCommandBufferHandle cmd,
                                                     const TerrainWorldHeightmap& hmap, const float* vp16,
                                                     float cam_x, float cam_y, float cam_z) {
    if (!batched_wireframe_ready_) return;
    GpuPassView pv = GpuPassView::FromRaw(rp, cmd);
    pv.BindPipeline(&batched_wireframe_pipeline_);

    struct TerrainBatchUBO {
        float vp[16];
        float height_range[4];
        float cam_pos_pad[4];
    } ubo{};
    std::memcpy(ubo.vp, vp16, 64);
    ubo.height_range[0] = hmap.HeightMin();
    ubo.height_range[1] = hmap.HeightMax();
    ubo.height_range[2] = hmap.WorldExtent();
    ubo.height_range[3] = (float)hmap.Resolution();
    ubo.cam_pos_pad[0] = cam_x;
    ubo.cam_pos_pad[1] = cam_y;
    ubo.cam_pos_pad[2] = cam_z;
    ubo.cam_pos_pad[3] = 0.f;
    pv.PushVertexUniforms(0, &ubo, sizeof(ubo));

    SDL_GPUTextureSamplerBinding samp[3] = {
        { hmap.Texture(), hmap.Sampler() },
        { hmap.NormalTexture(), hmap.NormalSampler() },
        { node_data_tex_, node_data_sampler_ },
    };
    pv.BindVertexSamplers(0, samp, 3);
}

void TerrainQuadtreeRenderer::DrawBatchedBoundary(SDL_GPURenderPass* rp, md::GpuCommandBufferHandle cmd, int count) {
    if (!batched_wireframe_ready_ || count <= 0) return;
    if (count > kMaxBatchedNodes) count = kMaxBatchedNodes;
    GpuPassView pv = GpuPassView::FromRaw(rp, cmd);
    // No index buffer -- terrain_quadtree_boundary.vert generates all 8
    // corner vertices/instance from gl_VertexIndex + nodeDataTex directly.
    pv.Draw(8, (uint32_t)count, 0, 0);
}
#endif // MD_SDL_GPU

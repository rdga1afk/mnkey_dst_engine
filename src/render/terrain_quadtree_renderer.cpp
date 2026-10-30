#include <chrono>
#include <monkey_dust/render/terrain_quadtree_renderer.h>
#include <monkey_dust/render/gpu_copy_pass.h>
#include <monkey_dust/render/gpu_pass_view.h>
#include <monkey_dust/render/gpu_hal_free_functions.h>
#include <monkey_dust/platform/md_log.h>
#include <monkey_dust/world/terrain_quadtree_mesh.h>
#include <monkey_dust/io/tiff.h>
#include <monkey_dust/world/biome_def.h>
#include <algorithm>
#include <cmath>
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
    pd.frag_path = "shaders/terrain_gbuffer_mini.frag"; // RESOLVE_OPT spatial-split reverted (81a091f) -- category dead, no samplers needed
    pd.frag_uniform_bufs = 0;
    pd.frag_samplers     = 0;
    pd.frag_storage_bufs = 0;
    pd.color_format = SDL_GPU_TEXTUREFORMAT_R16G16B16A16_UINT; // #142 phase 1b: 8 B/px G-buffer (terrain_gbuf_pack.glsl)
    if (!gbuffer_pipeline_.Create(pd)) {
        MD_LOG(MD_LOG_WARNING, "[TerrainQuadtreeRenderer] pipeline create failed");
        return false;
    }
    ready_ = true;
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
    pd.frag_path = "shaders/terrain_gbuffer_mini.frag"; // RESOLVE_OPT spatial-split reverted (81a091f), same as gbuffer_pipeline_
    pd.frag_uniform_bufs = 0;
    pd.frag_samplers     = 0; // category dead (see .frag's doc comment) -- no samplers needed
    pd.frag_storage_bufs = 0;
    pd.color_format = SDL_GPU_TEXTUREFORMAT_R16G16B16A16_UINT; // #142 phase 1b: 8 B/px G-buffer (terrain_gbuf_pack.glsl)
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
                                            const TerrainRenderer& /*ground*/) {
    // RESOLVE_OPT spatial-split reverted (81a091f) -- used to bind 3
    // samplers here for terrain_gbuffer_mini.frag's now-dead category
    // computation; ground param kept for call-site/header compatibility.
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
}

bool TerrainQuadtreeRenderer::InitDepthOnly(md::GpuDeviceHandle /*dev*/) {
    // Same vert_path/frag_path/sampler counts as gbuffer_pipeline_ --
    // deliberately, see this method's own header doc comment (codegen-
    // divergence risk if the frag shader differs from the color pass).
    {
        GpuPipeline::Desc pd;
        pd.layout.count       = 0;
        pd.raster.depth_test  = true;
        pd.raster.depth_write = true;
        pd.raster.cull_back   = false; // skirt quads face outward on all 4 borders
        pd.has_depth_target   = true;
        pd.depth_only         = true;
        pd.vert_uniform_bufs  = 1;
        pd.vert_samplers      = 2;
        pd.vert_path = "shaders/terrain_quadtree.vert";
        pd.frag_path = "shaders/terrain_gbuffer_mini.frag";
        pd.frag_uniform_bufs = 0;
        pd.frag_samplers     = 0;
        pd.frag_storage_bufs = 0;
        if (!gbuffer_depth_only_pipeline_.Create(pd)) {
            MD_LOG(MD_LOG_WARNING, "[TerrainQuadtreeRenderer] depth-only pipeline create failed");
            return false;
        }
        depth_only_ready_ = true;
    }
    {
        GpuPipeline::Desc pd;
        pd.layout.count       = 0;
        pd.raster.depth_test  = true;
        pd.raster.depth_write = true;
        pd.raster.cull_back   = false;
        pd.has_depth_target   = true;
        pd.depth_only         = true;
        pd.vert_uniform_bufs  = 1;
        pd.vert_samplers      = 3;
        pd.vert_path = "shaders/terrain_quadtree_batched.vert";
        pd.frag_path = "shaders/terrain_gbuffer_mini.frag";
        pd.frag_uniform_bufs = 0;
        pd.frag_samplers     = 0;
        pd.frag_storage_bufs = 0;
        if (!batched_depth_only_pipeline_.Create(pd)) {
            MD_LOG(MD_LOG_WARNING, "[TerrainQuadtreeRenderer] batched depth-only pipeline create failed");
            return false;
        }
        batched_depth_only_ready_ = true;
    }
    return true;
}

void TerrainQuadtreeRenderer::DrawNodeDepthOnly(SDL_GPURenderPass* rp, md::GpuCommandBufferHandle cmd,
                                                 const TerrainWorldHeightmap& hmap, const float* vp16,
                                                 const TerrainQuadtree::VisibleNode& node,
                                                 float cam_x, float cam_y, float cam_z) {
    if (!depth_only_ready_) return;
    constexpr float kPatchQuads = 16.0f;
    float texelSize = node.size / kPatchQuads;

    GpuPassView pv = GpuPassView::FromRaw(rp, cmd);
    pv.BindPipeline(&gbuffer_depth_only_pipeline_);

    TerrainQuadtreeUBO ubo{};
    std::memcpy(ubo.vp, vp16, 64);
    ubo.origin_size_texel_morph[0] = node.origin_x;
    ubo.origin_size_texel_morph[1] = node.origin_z;
    ubo.origin_size_texel_morph[2] = texelSize;
    ubo.origin_size_texel_morph[3] = 0.f;
    ubo.height_range[0] = hmap.HeightMin();
    ubo.height_range[1] = hmap.HeightMax();
    ubo.height_range[2] = hmap.WorldExtent();
    ubo.height_range[3] = (float)hmap.Resolution();
    ubo.cam_pos_skirt[0] = cam_x;
    ubo.cam_pos_skirt[1] = cam_y;
    ubo.cam_pos_skirt[2] = cam_z;
    ubo.cam_pos_skirt[3] = 0.f;
    pv.PushVertexUniforms(0, &ubo, sizeof(ubo));

    SDL_GPUTextureSamplerBinding samp[2] = {
        { hmap.Texture(), hmap.Sampler() },
        { hmap.NormalTexture(), hmap.NormalSampler() },
    };
    pv.BindVertexSamplers(0, samp, 2);

    pv.BindIndexBuffer(&filled_ibo_, SDL_GPU_INDEXELEMENTSIZE_32BIT);
    pv.DrawIndexed(filled_index_count_, 1, 0, 0, 0);
}

void TerrainQuadtreeRenderer::BeginBatchedDepthOnly(SDL_GPURenderPass* rp, md::GpuCommandBufferHandle cmd,
                                                     const TerrainWorldHeightmap& hmap, const float* vp16,
                                                     float cam_x, float cam_y, float cam_z) {
    if (!batched_depth_only_ready_) return;
    GpuPassView pv = GpuPassView::FromRaw(rp, cmd);
    pv.BindPipeline(&batched_depth_only_pipeline_);

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

void TerrainQuadtreeRenderer::DrawBatched(SDL_GPURenderPass* rp, md::GpuCommandBufferHandle cmd, int count) {
    if (!batched_ready_ || count <= 0) return;
    if (count > kMaxBatchedNodes) count = kMaxBatchedNodes;
    GpuPassView pv = GpuPassView::FromRaw(rp, cmd);

    pv.BindIndexBuffer(&filled_ibo_, SDL_GPU_INDEXELEMENTSIZE_32BIT);
    pv.DrawIndexed(filled_index_count_, (uint32_t)count, 0, 0, 0);
}


// ───────────────────────────── #154 phase 3: LOD ─────────────────────────────
namespace {
constexpr int   kFineW        = 1536;       // fine height window side, cells (MeitouClient: 1536 cells, recentred past 15 %)
constexpr float kCellM        = 1.8f;       // fullmap.tif spacing
constexpr float kHeightScaleM = 980.0f;     // metres at raw 65535 (terrain_gen_atlas_io.cpp TERRAIN_HEIGHT_SCALE_M)
constexpr int   kTifSide      = 16385;
struct LodUBO { float vp[16]; float height_range[4]; float cam_pos_pad[4]; float fine_win[4]; float flags[4]; int32_t wo[3][4]; };   // flags.x = 1: nodes covered by a normal window read their normal there; wo = origin (stride cells) of the world window of stride level 2i (xy) and 2i+1 (zw)
struct LodNormUBO { int32_t win[4]; int32_t lat[4]; float hr[4]; };
} // namespace

bool TerrainQuadtreeRenderer::InitLod(md::GpuDeviceHandle dev, const TerrainWorldHeightmap& hmap) {
    if (!dev || !batched_ready_) return false;
    const std::string kdir = BiomeRegistry::ResolveKenshiDir();
    if (kdir.empty()) {
        MD_LOG(MD_LOG_ERROR, "[TerrainQuadtreeRenderer] terrain LOD unavailable: Kenshi directory not found (set KENSHI_DIR); the flat tiling is used");
        lod_init_failed_ = true;
        return false;
    }
    tif_path_ = kdir + "/data/newland/land/fullmap.tif";
    height_min_ = hmap.HeightMin();
    height_range_ = std::max(hmap.HeightMax() - hmap.HeightMin(), 1.f);

    // Index buffers (16- and 64-quad patches): the filled grid (same as the flat path) + 4 skirt strips. Skirt bottom vertices are ids (cells+1)^2 + side * (cells+1) + k.
    auto build_ibo = [](int cells, GpuStaticBuffer& ibo, uint32_t& count) {
        const int g = cells + 1;
        std::vector<uint32_t> idx;
        idx.reserve((size_t)(cells * cells + 4 * cells) * 6);
        auto reg = [g](int col, int row) { return (uint32_t)(row * g + col); };
        for (int row = 0; row < cells; ++row)
            for (int col = 0; col < cells; ++col) {
                uint32_t i0 = reg(col, row), i1 = reg(col + 1, row), i2 = reg(col, row + 1), i3 = reg(col + 1, row + 1);
                idx.insert(idx.end(), { i0, i2, i1, i1, i2, i3 });
            }
        auto border = [&](int side, int k) -> uint32_t {
            switch (side) { case 0: return reg(k, 0); case 1: return reg(k, cells); case 2: return reg(0, k); default: return reg(cells, k); }
        };
        for (int side = 0; side < 4; ++side)
            for (int k = 0; k < cells; ++k) {
                uint32_t a = border(side, k), b = border(side, k + 1);
                uint32_t a2 = (uint32_t)(g * g + side * g + k), b2 = a2 + 1;
                idx.insert(idx.end(), { a, a2, b, b, a2, b2 });
            }
        ibo.Init(0x8893u /*GL_ELEMENT_ARRAY_BUFFER*/, idx.data(), idx.size() * sizeof(uint32_t));
        count = (uint32_t)idx.size();
    };
    build_ibo(16, lod_ibo_, lod_index_count_);
    build_ibo(64, lod_ibo64_, lod_index_count64_);

    // Fine height window texture (R16_UNORM, same normalisation as the world heightmap).
    SDL_GPUTextureCreateInfo ti{};
    ti.type = SDL_GPU_TEXTURETYPE_2D; ti.format = SDL_GPU_TEXTUREFORMAT_R16_UNORM; ti.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
    ti.width = kFineW; ti.height = kFineW; ti.layer_count_or_depth = 1; ti.num_levels = 1;
    fine_tex_ = GpuCreateTexture(dev, &ti);
    if (!fine_tex_) { MD_LOG(MD_LOG_WARNING, "[TerrainQuadtreeRenderer] fine height window create failed: %s", SDL_GetError()); return false; }
    SDL_GPUSamplerCreateInfo si{};
    si.min_filter = SDL_GPU_FILTER_NEAREST; si.mag_filter = SDL_GPU_FILTER_NEAREST; si.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
    si.address_mode_u = si.address_mode_v = si.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    fine_sampler_ = GpuCreateSampler(dev, &si);
    if (!fine_sampler_) return false;

    {   // #207: the normal windows (bound in every case; 1x1 placeholders when the switch is off)
        const char* e = getenv("MD_LOD_NORMTEX"); lod_normtex_ = !(e && e[0] == '0');   // default on (owner 28.10, #207; 0 px on the 17 views, -7.8 % J/frame at cap 45); MD_LOD_NORMTEX=0 turns it off
        world_cell_m_ = hmap.WorldExtent() / (float)(hmap.Resolution() - 1); world_max_cell_ = hmap.Resolution() - 1;
        for (auto& o : wnorm_o_) { o[0] = o[1] = -1; }
        SDL_GPUTextureCreateInfo ni{};
        ni.type = SDL_GPU_TEXTURETYPE_2D; ni.format = SDL_GPU_TEXTUREFORMAT_R32G32B32A32_FLOAT;
        ni.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_WRITE;
        ni.width = lod_normtex_ ? kFineW : 1; ni.height = ni.width; ni.layer_count_or_depth = 1; ni.num_levels = 1;
        fine_norm_tex_ = GpuCreateTexture(dev, &ni);
        ni.type = SDL_GPU_TEXTURETYPE_2D_ARRAY; ni.width = lod_normtex_ ? kWSide : 1; ni.height = ni.width; ni.layer_count_or_depth = lod_normtex_ ? kWLevels : 1;
        world_norm_tex_ = GpuCreateTexture(dev, &ni);
        if (!fine_norm_tex_ || !world_norm_tex_) { MD_LOG(MD_LOG_WARNING, "[TerrainQuadtreeRenderer] LOD normal window create failed: %s", SDL_GetError()); return false; }
        if (lod_normtex_) {
            GpuComputePipeline::Desc cd{};
            cd.glsl_path = "shaders/terrain_lod_normals.comp"; cd.num_uniform_buffers = 1; cd.num_samplers = 1; cd.num_readwrite_storage_textures = 1;
            cd.threadcount_x = 8; cd.threadcount_y = 8; cd.threadcount_z = 1;
            lod_norm_ready_ = lod_norm_pipeline_.Create(cd);
            MD_LOG(MD_LOG_INFO, "[TerrainQuadtreeRenderer] LOD normal windows %s: fine %dx%d + world %dx%d x %d strides, RGBA32F = %d MB",
                   lod_norm_ready_ ? "on" : "FAILED (old path)", kFineW, kFineW, kWSide, kWSide, kWLevels, (int)(((size_t)kFineW * kFineW + (size_t)kWSide * kWSide * kWLevels) * 16 >> 20));
        }
    }
    GpuPipeline::Desc pd;
    pd.layout.count = 0;
    pd.raster.depth_test = true; pd.raster.depth_write = true; pd.raster.cull_back = false;
    pd.has_depth_target = true;
    pd.vert_uniform_bufs = 1;
    pd.vert_samplers = 5; // heightTex, fineTex, nodeDataTex, fineNormTex, worldNormTex
    pd.vert_path = "shaders/terrain_quadtree_batched_lod.vert";
    pd.frag_path = "shaders/terrain_gbuffer_mini.frag";
    pd.frag_uniform_bufs = 0; pd.frag_samplers = 0; pd.frag_storage_bufs = 0;
    pd.color_format = SDL_GPU_TEXTUREFORMAT_R16G16B16A16_UINT;
    if (!lod_pipeline_.Create(pd)) { MD_LOG(MD_LOG_WARNING, "[TerrainQuadtreeRenderer] lod pipeline create failed"); return false; }
    lod_ready_ = true;
    // the tables (a read of the whole fullmap.tif, ~3 s) are built from here, in parallel with the rest of the start-up: the first frame then already draws the LOD, not the flat tiling
    lod_started_ = true; lod_build_t0_ = std::chrono::steady_clock::now(); lod_tree_.BuildAsync(tif_path_.c_str(), hmap.WorldExtent(), 980.0f, lod_cells_);
    return true;
}

void TerrainQuadtreeRenderer::UploadLodNodes(md::GpuDeviceHandle dev, SDL_GPUCopyPass* cp, const md::TerrainLodNode* nodes, int count,
                                              float cam_x, float cam_z) {
    if (!lod_ready_ || !dev || !cp) return;
    // 1. fine window: recentre when the camera leaves the middle 70 % (or on the first frame). The read + normalisation run on a worker; the old window
    // stays in use until the new one is uploaded.
    {
        if (fine_job_active_ && fine_job_.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
            std::vector<uint16_t> norm = fine_job_.get();
            fine_job_active_ = false;
            if (norm.size() == (size_t)kFineW * kFineW) {
                const Uint32 bytes = (Uint32)(norm.size() * sizeof(uint16_t));
                SDL_GPUTransferBufferCreateInfo tbi{}; tbi.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD; tbi.size = bytes;
                if (SDL_GPUTransferBuffer* tb = GpuCreateTransferBuffer(dev, &tbi)) {
                    if (void* map = GpuMapTransfer(tb, false)) std::memcpy(map, norm.data(), bytes);
                    GpuUnmapTransfer(tb);
                    SDL_GPUTextureTransferInfo src{}; src.transfer_buffer = tb; src.pixels_per_row = kFineW; src.rows_per_layer = kFineW;
                    SDL_GPUTextureRegion dst{}; dst.texture = fine_tex_; dst.w = kFineW; dst.h = kFineW; dst.d = 1;
                    GpuCopyPass::FromRaw(cp, nullptr).UploadTexture(src, dst, false);
                    GpuReleaseTransferBuffer(dev, tb);
                    fine_ox_ = fine_job_ox_; fine_oz_ = fine_job_oz_;
                    if (lod_norm_ready_) { fine_norm_pending_ = true; fine_norm_ok_ = false; }
                }
            } else {
                MD_LOG(MD_LOG_WARNING, "[TerrainQuadtreeRenderer] fine window read failed");
            }
        }
        const int cx = (int)std::lround(cam_x / kCellM), cz = (int)std::lround(cam_z / kCellM);
        const int lim = fine_lim_cells_;
        if (!fine_job_active_ && (fine_ox_ < -100000 || std::abs(cx - (fine_ox_ + kFineW / 2)) > lim || std::abs(cz - (fine_oz_ + kFineW / 2)) > lim)) {
            const int ox = std::min(std::max(cx - kFineW / 2, 0), kTifSide - kFineW);
            const int oz = std::min(std::max(cz - kFineW / 2, 0), kTifSide - kFineW);
            fine_job_ox_ = ox; fine_job_oz_ = oz; fine_job_active_ = true;
            fine_job_ = std::async(std::launch::async, [path = tif_path_, ox, oz, hmin = height_min_, hrange = height_range_]() {
                std::vector<uint16_t> norm;
                const auto t0 = std::chrono::steady_clock::now();
                md::tiff::Image img; std::string err; std::vector<uint16_t> win;
                if (img.Open(path.c_str(), err) && img.Window((uint32_t)ox, (uint32_t)oz, kFineW, kFineW, 1, win, err)) {
                    norm.resize(win.size());
                    const float k = kHeightScaleM / 65535.f;
                    for (size_t i = 0; i < win.size(); ++i) {
                        float v = ((float)win[i] * k - hmin) / hrange * 65535.f + 0.5f;
                        norm[i] = (uint16_t)std::min(std::max(v, 0.f), 65535.f);
                    }
                } else {
                    MD_LOG(MD_LOG_WARNING, "[TerrainQuadtreeRenderer] fine window read failed: %s", err.c_str());
                }
                MD_LOG(MD_LOG_INFO, "[TerrainQuadtreeRenderer] fine window (%d, %d) read in %lld ms", ox, oz, (long long)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count());
                return norm;
            });
        }
    }
    if (count <= 0) return;
    if (count > kMaxBatchedNodes) count = kMaxBatchedNodes;
    // 1b. #207: the world stride windows follow the camera (recentred past 30 % of their half side); a moved window is rebuilt by BakeLodNormals this frame
    if (lod_norm_ready_) {
        for (int k = 0; k < kWLevels; ++k) {
            const int stride = 1 << k;
            const int ccx = (int)std::lround(cam_x / (world_cell_m_ * stride)), ccz = (int)std::lround(cam_z / (world_cell_m_ * stride));
            const int hi = std::max(world_max_cell_ / stride - kWSide + 1, 0);
            int& ox = wnorm_o_[k][0]; int& oz = wnorm_o_[k][1];
            const int nox = std::min(std::max(ccx - kWSide / 2, 0), hi), noz = std::min(std::max(ccz - kWSide / 2, 0), hi);   // a window clamped at the world edge (or covering a whole coarse level) does not move
            if (ox < 0 || ((std::abs(ccx - (ox + kWSide / 2)) > kWSide * 3 / 10 || std::abs(ccz - (oz + kWSide / 2)) > kWSide * 3 / 10) && (nox != ox || noz != oz))) {
                ox = nox; oz = noz;
                wnorm_pending_[k] = true;
            }
        }
    }
    // 2. node data: texel A = (origin x, origin z, vertex spacing, morph), texel B = (level, skirt depth, 1 = fine window, 1 + world stride level when a world normal window covers the node)
    static float staging[kMaxBatchedNodes * 2 * 4];
    for (int i = 0; i < count; ++i) {
        const float spacing = nodes[i].size / (float)lod_cells_;
        float* a = staging + (size_t)i * 8;
        float* b = a + 4;
        a[0] = nodes[i].origin_x; a[1] = nodes[i].origin_z; a[2] = spacing; a[3] = nodes[i].morph;
        const float m = 3.f * kCellM; // ring of neighbours the normals / morph read, plus a margin
        const float wx0 = (float)fine_ox_ * kCellM, wz0 = (float)fine_oz_ * kCellM, wx1 = wx0 + (kFineW - 1) * kCellM, wz1 = wz0 + (kFineW - 1) * kCellM;
        const bool fine = spacing < 3.5f && fine_ox_ >= 0 && nodes[i].origin_x - m >= wx0 && nodes[i].origin_z - m >= wz0 &&
                          nodes[i].origin_x + nodes[i].size + m <= wx1 && nodes[i].origin_z + nodes[i].size + m <= wz1;
        b[0] = (float)nodes[i].level; b[1] = nodes[i].skirt_depth; b[2] = fine ? 1.f : 0.f; b[3] = 0.f;
        if (lod_norm_ready_ && spacing > 3.5f) {   // same lattice rule as the vertex shader: stride = round(spacing / 3.6), the node's cells are multiples of the stride
            const int stride = (int)(spacing / 3.6f + 0.5f);
            int k = 0; while ((1 << k) < stride && k < kWLevels) ++k;
            if (k < kWLevels && (1 << k) == stride) {
                const long c0 = std::lround(nodes[i].origin_x / world_cell_m_), r0 = std::lround(nodes[i].origin_z / world_cell_m_);
                if (c0 % stride == 0 && r0 % stride == 0) {
                    const int lx = (int)(c0 / stride) - wnorm_o_[k][0], lz = (int)(r0 / stride) - wnorm_o_[k][1];
                    if (lx - 2 >= 0 && lz - 2 >= 0 && lx + lod_cells_ + 2 <= kWSide - 1 && lz + lod_cells_ + 2 <= kWSide - 1) b[3] = (float)(k + 1);
                }
            }
        }
    }
    int texel_count = count * 2;
    int rows = (texel_count + kNodeDataTexWidth - 1) / kNodeDataTexWidth;
    Uint32 upload_bytes = (Uint32)((size_t)count * 8 * sizeof(float));
    Uint32 region_bytes = (Uint32)kNodeDataTexWidth * (Uint32)rows * 4 * sizeof(float);
    SDL_GPUTransferBufferCreateInfo tbi{}; tbi.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD; tbi.size = region_bytes;
    SDL_GPUTransferBuffer* tb = GpuCreateTransferBuffer(dev, &tbi);
    if (!tb) return;
    if (void* map = GpuMapTransfer(tb, false)) {
        if (region_bytes > upload_bytes) std::memset(map, 0, region_bytes);
        std::memcpy(map, staging, upload_bytes);
    }
    GpuUnmapTransfer(tb);
    SDL_GPUTextureTransferInfo src{}; src.transfer_buffer = tb; src.pixels_per_row = (Uint32)kNodeDataTexWidth; src.rows_per_layer = (Uint32)rows;
    SDL_GPUTextureRegion dst{}; dst.texture = node_data_tex_; dst.w = (Uint32)kNodeDataTexWidth; dst.h = (Uint32)rows; dst.d = 1;
    GpuCopyPass::FromRaw(cp, nullptr).UploadTexture(src, dst, false);
    GpuReleaseTransferBuffer(dev, tb);
}

bool TerrainQuadtreeRenderer::LodWindowsIdle(float cam_x, float cam_z) const {
    if (!lod_ready_ || fine_job_active_ || fine_ox_ < 0 || fine_norm_pending_) return false;
    for (int k = 0; k < kWLevels; ++k) if (wnorm_pending_[k]) return false;
    const int cx = (int)std::lround(cam_x / kCellM), cz = (int)std::lround(cam_z / kCellM);
    return std::abs(cx - (fine_ox_ + kFineW / 2)) <= fine_lim_cells_ && std::abs(cz - (fine_oz_ + kFineW / 2)) <= fine_lim_cells_;
}

void TerrainQuadtreeRenderer::BakeLodNormals(SDL_GPUCommandBuffer* cmd, const TerrainWorldHeightmap& hmap) {
    if (!lod_norm_ready_ || !cmd) return;
    auto run = [&](SDL_GPUTexture* dst, int layer, SDL_GPUTexture* src, SDL_GPUSampler* sampler, const LodNormUBO& ub, int side) {
        GpuComputePass::StorageBindings sb; sb.cmd = cmd;
        sb.rw_textures[0].texture = dst; sb.rw_textures[0].layer = (Uint32)layer; sb.rw_textures[0].cycle = false; sb.num_rw_textures = 1;
        GpuComputePass pass; pass.Begin(&lod_norm_pipeline_, sb);
        if (!pass.SDLPass()) return false;
        SDL_GPUTextureSamplerBinding sbind{}; sbind.texture = src; sbind.sampler = sampler;
        pass.BindSamplers(0, &sbind, 1); pass.PushUniforms(0, &ub, sizeof(ub));
        pass.Dispatch((Uint32)((side + 7) / 8), (Uint32)((side + 7) / 8), 1); pass.End();
        return true;
    };
    int jobs = 0; const auto t0 = std::chrono::steady_clock::now();
    if (fine_norm_pending_ && fine_ox_ >= 0) {
        LodNormUBO ub{}; ub.win[0] = 0; ub.win[1] = 0; ub.win[2] = 1; ub.win[3] = kFineW; ub.lat[0] = kFineW - 1;
        ub.hr[0] = height_min_; ub.hr[1] = height_min_ + height_range_; ub.hr[2] = kCellM;
        if (run(fine_norm_tex_, 0, fine_tex_, fine_sampler_, ub, kFineW)) { fine_norm_ok_ = true; ++jobs; }
        fine_norm_pending_ = false;
    }
    for (int k = 0; k < kWLevels; ++k) {
        if (!wnorm_pending_[k]) continue;
        wnorm_pending_[k] = false;
        const int stride = 1 << k;
        LodNormUBO ub{}; ub.win[0] = wnorm_o_[k][0] * stride; ub.win[1] = wnorm_o_[k][1] * stride; ub.win[2] = stride; ub.win[3] = kWSide; ub.lat[0] = world_max_cell_;
        ub.hr[0] = hmap.HeightMin(); ub.hr[1] = hmap.HeightMax(); ub.hr[2] = world_cell_m_ * (float)stride;
        if (run(world_norm_tex_, k, hmap.Texture(), hmap.Sampler(), ub, kWSide)) ++jobs;
    }
    if (jobs) ++lod_norm_bakes_;
    if (jobs) MD_LOG(MD_LOG_INFO, "[TerrainQuadtreeRenderer] LOD normal windows baked: %d (cpu side %lld us)", jobs, (long long)std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0).count());
}

void TerrainQuadtreeRenderer::BeginBatchedLod(SDL_GPURenderPass* rp, md::GpuCommandBufferHandle cmd, const TerrainWorldHeightmap& hmap,
                                               const float* vp16, float cam_x, float cam_y, float cam_z) {
    if (!lod_ready_) return;
    GpuPassView pv = GpuPassView::FromRaw(rp, cmd);
    pv.BindPipeline(&lod_pipeline_);
    LodUBO ubo{};
    std::memcpy(ubo.vp, vp16, 64);
    ubo.height_range[0] = hmap.HeightMin(); ubo.height_range[1] = hmap.HeightMax();
    ubo.height_range[2] = hmap.WorldExtent(); ubo.height_range[3] = (float)hmap.Resolution();
    ubo.cam_pos_pad[0] = cam_x; ubo.cam_pos_pad[1] = cam_y; ubo.cam_pos_pad[2] = cam_z; ubo.cam_pos_pad[3] = (float)lod_cells_;
    ubo.fine_win[0] = (float)fine_ox_; ubo.fine_win[1] = (float)fine_oz_; ubo.fine_win[2] = (float)kFineW; ubo.fine_win[3] = kCellM;
    ubo.flags[0] = (lod_normtex_ && lod_norm_ready_ && fine_norm_ok_) ? 1.f : 0.f;
    for (int k = 0; k < kWLevels; ++k) ubo.wo[k >> 1][(k & 1) * 2] = wnorm_o_[k][0], ubo.wo[k >> 1][(k & 1) * 2 + 1] = wnorm_o_[k][1];
    pv.PushVertexUniforms(0, &ubo, sizeof(ubo));
    SDL_GPUTextureSamplerBinding samp[5] = {
        { hmap.Texture(), hmap.Sampler() },
        { fine_tex_, fine_sampler_ },
        { node_data_tex_, node_data_sampler_ },
        { fine_norm_tex_, fine_sampler_ },
        { world_norm_tex_, fine_sampler_ },
    };
    pv.BindVertexSamplers(0, samp, 5);
}

void TerrainQuadtreeRenderer::DrawBatchedLod(SDL_GPURenderPass* rp, md::GpuCommandBufferHandle cmd, int count) {
    if (!lod_ready_ || count <= 0) return;
    if (count > kMaxBatchedNodes) count = kMaxBatchedNodes;
    GpuPassView pv = GpuPassView::FromRaw(rp, cmd);
    const bool c64 = lod_cells_ == 64;
    pv.BindIndexBuffer(c64 ? &lod_ibo64_ : &lod_ibo_, SDL_GPU_INDEXELEMENTSIZE_32BIT);
    pv.DrawIndexed(c64 ? lod_index_count64_ : lod_index_count_, (uint32_t)count, 0, 0, 0);
}

void TerrainQuadtreeRenderer::Shutdown(md::GpuDeviceHandle dev) {
    gbuffer_pipeline_.Destroy();
    if (wireframe_ready_) wireframe_pipeline_.Destroy();
    if (batched_wireframe_ready_) batched_wireframe_pipeline_.Destroy();
    if (depth_only_ready_) gbuffer_depth_only_pipeline_.Destroy();
    if (batched_depth_only_ready_) batched_depth_only_pipeline_.Destroy();
    if (lod_ready_) { lod_pipeline_.Destroy(); if (lod_norm_ready_) lod_norm_pipeline_.Destroy(); lod_ready_ = false; }
    if (batched_ready_) {
        batched_pipeline_.Destroy();
        if (dev && node_data_tex_) GpuReleaseTexture(dev, node_data_tex_);
        if (dev && node_data_sampler_) GpuReleaseSampler(dev, node_data_sampler_);
        node_data_tex_ = nullptr;
        node_data_sampler_ = nullptr;
    }
    filled_ibo_.Shutdown();
    ready_ = false;
    wireframe_ready_ = false;
    batched_ready_ = false;
    batched_wireframe_ready_ = false;
    depth_only_ready_ = false;
    batched_depth_only_ready_ = false;
}

void TerrainQuadtreeRenderer::DrawNode(SDL_GPURenderPass* rp, md::GpuCommandBufferHandle cmd,
                                        const TerrainWorldHeightmap& hmap, const float* vp16,
                                        const TerrainQuadtree::VisibleNode& node,
                                        float cam_x, float cam_y, float cam_z,
                                        const TerrainRenderer& /*ground*/) {
    // RESOLVE_OPT spatial-split reverted (81a091f) -- used to bind 3
    // samplers here for terrain_gbuffer_mini.frag's now-dead category
    // computation; ground param kept for call-site/header compatibility.
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
    // format main colour target, same contract as the G-buffer pipelines.
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

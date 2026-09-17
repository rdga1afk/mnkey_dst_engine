#ifdef MD_SDL_GPU
#include <monkey_dust/render/terrain_tin_renderer.h>
#include <monkey_dust/render/gpu_pass_view.h>
#include <monkey_dust/platform/md_log.h>
#include <cstring>

namespace {
// Field order matches shaders/terrain_tin.vert's TerrainTinUBO exactly (std140).
struct TerrainTinUBO {
    float vp[16];
    float zone_origin[4]; // x=zone_origin_x, y=zone_origin_z, z/w unused
    float cam_pos[4];     // xyz=camera world position, w unused
};
static_assert(sizeof(TerrainTinUBO) == 64 + 16 + 16, "TerrainTinUBO size mismatch");
} // namespace

bool TerrainTinRenderer::Init(md::GpuDeviceHandle /*dev*/) {
    GpuPipeline::Desc pd;
    // TerrainTinVertex: pos(loc=0,off=0,F3) + normal(loc=1,off=12,F3), stride=24.
    pd.layout.count      = 2;
    pd.layout.stride     = 24;
    pd.layout.attribs[0] = { 0, 0,  GpuAttribFmt::F3 };
    pd.layout.attribs[1] = { 1, 12, GpuAttribFmt::F3 };

    pd.raster.depth_test  = true;
    pd.raster.depth_write = true;
    pd.raster.cull_back   = false; // match TerrainQuadtreeRenderer's own choice
    pd.has_depth_target   = true;

    pd.vert_uniform_bufs = 1;
    pd.vert_samplers      = 0; // real vertex buffer, no VTF sampling
    pd.vert_path = "shaders/terrain_tin.vert";
    pd.frag_path = "shaders/terrain_gbuffer_mini.frag"; // unmodified, same output contract
    pd.frag_uniform_bufs = 0;
    pd.frag_samplers     = 0;
    pd.frag_storage_bufs = 0;
    pd.color_format = SDL_GPU_TEXTUREFORMAT_R32G32B32A32_FLOAT;
    if (!gbuffer_pipeline_.Create(pd)) {
        MD_LOG(MD_LOG_WARNING, "[TerrainTinRenderer] pipeline create failed");
        return false;
    }
    ready_ = true;
    return true;
}

void TerrainTinRenderer::DrawMesh(SDL_GPURenderPass* rp, md::GpuCommandBufferHandle cmd,
                                   const TerrainTinMesh& mesh, const float* vp16,
                                   float zone_origin_x, float zone_origin_z,
                                   float cam_x, float cam_y, float cam_z) {
    if (!ready_ || !mesh.IsReady()) return;
    if (!mesh.vbo.SDLBuffer() || !mesh.ibo.SDLBuffer()) return;

    GpuPassView pv = GpuPassView::FromRaw(rp, cmd);
    pv.BindPipeline(&gbuffer_pipeline_);
    pv.BindVertexBuffer(&mesh.vbo);

    TerrainTinUBO ubo{};
    std::memcpy(ubo.vp, vp16, 64);
    ubo.zone_origin[0] = zone_origin_x;
    ubo.zone_origin[1] = zone_origin_z;
    ubo.cam_pos[0] = cam_x;
    ubo.cam_pos[1] = cam_y;
    ubo.cam_pos[2] = cam_z;
    pv.PushVertexUniforms(0, &ubo, sizeof(ubo));

    pv.BindIndexBuffer(&mesh.ibo, SDL_GPU_INDEXELEMENTSIZE_32BIT);
    pv.DrawIndexed(mesh.index_count, 1, 0, 0, 0);
}

void TerrainTinRenderer::Shutdown(md::GpuDeviceHandle /*dev*/) {
    gbuffer_pipeline_.Destroy();
    ready_ = false;
}
#endif

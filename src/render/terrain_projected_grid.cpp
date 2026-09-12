#include <monkey_dust/render/terrain_projected_grid.h>
#ifdef MD_SDL_GPU
#include <monkey_dust/platform/md_log.h>
#include <monkey_dust/world/terrain_quadtree.h> // kGameplayMaxRenderDistance
#include <cstring>
#include <cmath>
#include <vector>

namespace {
constexpr int kGridW = 192; // must match shaders/terrain_projected_grid.vert's kGridW
constexpr int kGridH = 108; // must match shaders/terrain_projected_grid.vert's kGridH

// Field order matches shaders/terrain_projected_grid.vert's
// TerrainProjectedGridUBO exactly (std140). 128B total -- AT the Intel
// HD 520 push-uniform ceiling (.claude/rules/gpu-shader-debug.md), no
// spare bytes (docs/TERRAIN_PROJECTED_GRID.md Критичне виправлення #1).
struct TpgUBO {
    float vp[16];
    float corner_xz01[4];
    float corner_xz23[4];
    float height_range[4];
    float cam_pos_pad[4];
};
static_assert(sizeof(TpgUBO) == 128, "TpgUBO size mismatch");

// Manual clip->world unproject via the raw column-major float[16] --
// same hand-rolled matrix*vector pattern md_camera.h's MdWorldToScreen
// already uses for the opposite (world->clip) direction; no generic
// mat4*vec4 helper exists in math_types.h.
Vec3 UnprojectNDC(const Mat4& inv_vp, float ndc_x, float ndc_y, float ndc_z) {
    const float* m = mat4_ptr(inv_vp);
    float cx = m[0]*ndc_x + m[4]*ndc_y + m[8] *ndc_z + m[12];
    float cy = m[1]*ndc_x + m[5]*ndc_y + m[9] *ndc_z + m[13];
    float cz = m[2]*ndc_x + m[6]*ndc_y + m[10]*ndc_z + m[14];
    float cw = m[3]*ndc_x + m[7]*ndc_y + m[11]*ndc_z + m[15];
    if (fabsf(cw) < 1e-6f) cw = (cw < 0.f) ? -1e-6f : 1e-6f;
    float inv_w = 1.f / cw;
    return Vec3{ cx*inv_w, cy*inv_w, cz*inv_w };
}

struct CornerXZ { float x, z; bool capped; };

// Johanson (2004) camera-projector ray-plane intersection: unproject this
// screen corner's far-plane point, build a ray from the camera through
// it, intersect with the horizontal base plane y=y_base. Near-horizon
// pitch (or a plane the camera is looking away from) makes the ray
// nearly parallel to the plane or gives a negative/huge distance -- both
// clamped to max_dist rather than left to blow up to +-infinity (see
// docs/TERRAIN_PROJECTED_GRID.md §0.2 "Вироджені випадки").
CornerXZ ComputeCorner(const Mat4& inv_vp, Vec3 cam_pos, float ndc_x, float ndc_y,
                        float y_base, float max_dist) {
    Vec3 far_pt = UnprojectNDC(inv_vp, ndc_x, ndc_y, 1.0f);
    Vec3 dir = vec3_sub(far_pt, cam_pos);
    float dir_len = vec3_len(dir);
    if (dir_len < 1e-6f) dir_len = 1e-6f;
    dir = vec3_scale(dir, 1.f / dir_len);

    bool  capped = false;
    float t;
    if (fabsf(dir.y) < 1e-4f) {
        t = max_dist;
        capped = true;
    } else {
        t = (y_base - cam_pos.y) / dir.y;
        if (t <= 0.f || t > max_dist) {
            t = max_dist;
            capped = true;
        }
    }
    return { cam_pos.x + dir.x * t, cam_pos.z + dir.z * t, capped };
}
} // namespace

bool TerrainProjectedGrid::Init(md::GpuDeviceHandle /*dev*/) {
    std::vector<uint32_t> indices;
    indices.reserve((size_t)(kGridW - 1) * (kGridH - 1) * 6);
    auto idx = [](int col, int row) -> uint32_t { return (uint32_t)(row * kGridW + col); };
    // Same winding convention as terrain_quadtree_mesh.cpp's BuildTerrainQuadtreeMesh
    // (i0,i2,i1 / i1,i2,i3): i0=(c,r) i1=(c+1,r) i2=(c,r+1) i3=(c+1,r+1).
    for (int row = 0; row < kGridH - 1; ++row) {
        for (int col = 0; col < kGridW - 1; ++col) {
            uint32_t i0 = idx(col, row);
            uint32_t i1 = idx(col + 1, row);
            uint32_t i2 = idx(col, row + 1);
            uint32_t i3 = idx(col + 1, row + 1);
            indices.push_back(i0); indices.push_back(i2); indices.push_back(i1);
            indices.push_back(i1); indices.push_back(i2); indices.push_back(i3);
        }
    }

    grid_ibo_.Init(0x8893u /*GL_ELEMENT_ARRAY_BUFFER*/, indices.data(),
                    (uint32_t)(indices.size() * sizeof(uint32_t)));
    grid_index_count_ = (uint32_t)indices.size();

    GpuPipeline::Desc pd;
    pd.layout.count       = 0; // vertex-buffer-less -- gl_VertexIndex comes from the bound IBO alone
    pd.raster.depth_test  = true;
    pd.raster.depth_write = true;
    pd.raster.cull_back   = false;
    pd.has_depth_target   = true;
    pd.vert_uniform_bufs  = 1;
    pd.vert_samplers      = 2; // heightTex + normalTex (world-wide, same as terrain_quadtree.vert)
    pd.vert_path = "shaders/terrain_projected_grid.vert";
    pd.frag_path = "shaders/terrain_gbuffer_mini.frag"; // unmodified, same output contract
    pd.frag_uniform_bufs = 0;
    pd.frag_samplers     = 0;
    pd.frag_storage_bufs = 0;
    pd.color_format = SDL_GPU_TEXTUREFORMAT_R32G32B32A32_FLOAT;
    if (!gbuffer_pipeline_.Create(pd)) {
        MD_LOG(MD_LOG_WARNING, "[TerrainProjectedGrid] pipeline create failed");
        return false;
    }
    ready_ = true;
    return true;
}

void TerrainProjectedGrid::Shutdown(md::GpuDeviceHandle /*dev*/) {
    gbuffer_pipeline_.Destroy();
    grid_ibo_.Shutdown();
    ready_ = false;
}

void TerrainProjectedGrid::Draw(SDL_GPURenderPass* rp, md::GpuCommandBufferHandle cmd,
                                 const TerrainWorldHeightmap& hmap,
                                 const MdCamera& abs_cam, float aspect, float y_base) {
    if (!ready_) return;

    Mat4 vp     = abs_cam.ViewProjMatrix(aspect);
    Mat4 inv_vp = mat4_inv(vp);
    const float kMaxDist = TerrainQuadtree::kGameplayMaxRenderDistance;

    // Corner order matches the .vert's (u,v) convention exactly:
    // c0=(u=0,v=0) c1=(u=1,v=0) c2=(u=0,v=1) c3=(u=1,v=1).
    CornerXZ c0 = ComputeCorner(inv_vp, abs_cam.pos, -1.f, -1.f, y_base, kMaxDist);
    CornerXZ c1 = ComputeCorner(inv_vp, abs_cam.pos,  1.f, -1.f, y_base, kMaxDist);
    CornerXZ c2 = ComputeCorner(inv_vp, abs_cam.pos, -1.f,  1.f, y_base, kMaxDist);
    CornerXZ c3 = ComputeCorner(inv_vp, abs_cam.pos,  1.f,  1.f, y_base, kMaxDist);
    capped_corner_count_ = (c0.capped ? 1 : 0) + (c1.capped ? 1 : 0)
                          + (c2.capped ? 1 : 0) + (c3.capped ? 1 : 0);

    TpgUBO ubo{};
    std::memcpy(ubo.vp, mat4_ptr(vp), 64);
    ubo.corner_xz01[0] = c0.x; ubo.corner_xz01[1] = c0.z;
    ubo.corner_xz01[2] = c1.x; ubo.corner_xz01[3] = c1.z;
    ubo.corner_xz23[0] = c2.x; ubo.corner_xz23[1] = c2.z;
    ubo.corner_xz23[2] = c3.x; ubo.corner_xz23[3] = c3.z;
    ubo.height_range[0] = hmap.HeightMin();
    ubo.height_range[1] = hmap.HeightMax();
    ubo.height_range[2] = hmap.WorldExtent();
    ubo.height_range[3] = (float)hmap.Resolution();
    ubo.cam_pos_pad[0] = abs_cam.pos.x;
    ubo.cam_pos_pad[1] = abs_cam.pos.y;
    ubo.cam_pos_pad[2] = abs_cam.pos.z;
    ubo.cam_pos_pad[3] = 0.f;

    GpuPassView pv = GpuPassView::FromRaw(rp, cmd);
    pv.BindPipeline(&gbuffer_pipeline_);
    pv.PushVertexUniforms(0, &ubo, sizeof(ubo));

    SDL_GPUTextureSamplerBinding samp[2] = {
        { hmap.Texture(), hmap.Sampler() },
        { hmap.NormalTexture(), hmap.NormalSampler() },
    };
    pv.BindVertexSamplers(0, samp, 2);

    pv.BindIndexBuffer(&grid_ibo_, SDL_GPU_INDEXELEMENTSIZE_32BIT);
    pv.DrawIndexed(grid_index_count_, 1, 0, 0, 0);
}
#endif // MD_SDL_GPU

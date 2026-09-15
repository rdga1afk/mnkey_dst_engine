#include <monkey_dust/render/vegetation_renderer.h>
#include <monkey_dust/render/light_system.h>
#include <monkey_dust/render/gpu_pass_view.h>
#include <cstring>

bool VegetationRenderer::Init(const char* mesh_path) {
    mesh_.Init(mesh_path);
#ifdef MD_SDL_GPU
    pos_buf_.Init(MAX_INSTANCES * 16,
                  SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ);

    GpuPipeline::Desc d;
    d.vert_path         = "shaders/veg_billboard.vert";
    d.frag_path         = "shaders/veg_billboard.frag";
    d.vert_uniform_bufs = 1;   // BillUBO (set=1, b=0)
    d.vert_storage_bufs = 1;   // PosBuf  (set=0, b=0)
    d.frag_uniform_bufs = 0;
    d.frag_samplers     = 0;   // placeholder: no impostor texture
    d.layout.count      = 0;   // positions come from SSBO, no vertex input
    // Billboard faces camera — back face never visible, but disable culling for safety.
    d.raster.cull_back  = false;
    bill_pipe_.Create(d);
#endif
    return true;
}

void VegetationRenderer::Shutdown() {
    mesh_.Shutdown();
#ifdef MD_SDL_GPU
    bill_pipe_.Destroy();
    pos_buf_.Shutdown();
#endif
}

void VegetationRenderer::DrawMesh(
#ifdef MD_SDL_GPU
    SDL_GPURenderPass* rp, md::GpuCommandBufferHandle cmd,
#endif
    const float* pos3, int count, const float* vp16, const float* sun32,
    float scale, float anim_mode, float anim_time) {
    mesh_.DrawRaw(
#ifdef MD_SDL_GPU
        rp, cmd,
#endif
        pos3, count, vp16, sun32, scale, anim_mode, anim_time);
}

#ifdef MD_SDL_GPU
void VegetationRenderer::DrawBillboards(
    SDL_GPURenderPass* rp, md::GpuCommandBufferHandle cmd,
    const float* pos3, int count,
    const float* vp16, const float* cam_right3,
    const float* sun_col3, const float* ambient3) {
    if (!BillboardReady() || count <= 0 || !pos_buf_.SDLBuffer()) return;
    if (count > MAX_INSTANCES) count = MAX_INSTANCES;

    // Pack xyz positions as vec4 (w=1, unused by shader).
    static float packed[MAX_INSTANCES * 4];
    for (int i = 0; i < count; ++i) {
        packed[i*4+0] = pos3[i*3+0];
        packed[i*4+1] = pos3[i*3+1];
        packed[i*4+2] = pos3[i*3+2];
        packed[i*4+3] = 1.0f;
    }
    pos_buf_.UploadInCmd(cmd, packed, count * 16);

    GpuPassView pv = GpuPassView::FromRaw(rp, cmd);
    pv.BindPipeline(&bill_pipe_);
    SDL_GPUBuffer* stor = pos_buf_.SDLBuffer();
    pv.BindVertexStorageBuffers(0, &stor, 1);

    struct BillUBO {
        float vp[16];
        float cam_right[4];  // xyz=right, w=half_width_m
        float cam_up[4];     // xyz=world_up, w=height_m
        float sun_col[4];
        float ambient[4];
    } ubo;
    memcpy(ubo.vp, vp16, 64);
    ubo.cam_right[0] = cam_right3[0];
    ubo.cam_right[1] = cam_right3[1];
    ubo.cam_right[2] = cam_right3[2];
    ubo.cam_right[3] = BILLBOARD_W;
    ubo.cam_up[0] = 0.f; ubo.cam_up[1] = 1.f; ubo.cam_up[2] = 0.f;
    ubo.cam_up[3] = BILLBOARD_H;
    ubo.sun_col[0]=sun_col3[0]; ubo.sun_col[1]=sun_col3[1];
    ubo.sun_col[2]=sun_col3[2]; ubo.sun_col[3]=0.f;
    ubo.ambient[0]=ambient3[0]; ubo.ambient[1]=ambient3[1];
    ubo.ambient[2]=ambient3[2]; ubo.ambient[3]=0.f;
    pv.PushVertexUniforms(0, &ubo, sizeof(ubo));

    pv.Draw(6, (uint32_t)count, 0, 0);
}
#else
void VegetationRenderer::DrawBillboards(
    const float* /*pos3*/, int /*count*/,
    const float* /*vp16*/, const float* /*cam_right3*/,
    const float* /*sun_col3*/, const float* /*ambient3*/) {}
#endif

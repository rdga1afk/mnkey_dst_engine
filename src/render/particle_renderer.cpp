#include <monkey_dust/render/particle_renderer.h>
#include <monkey_dust/render/gpu_pass_view.h>


void ParticleRenderer::Init() {
    // Vertex layout: pos(F3) at offset 0, size(F1) at 12, color(U8x4_NORM) at 16.
    // Matches ParticleVertex { float x,y,z,size; uint8_t r,g,b,a; } stride=20.
    GpuPipeline::Desc desc;
    desc.vert_path        = "shaders/particle.vert";
    desc.frag_path        = "shaders/particle.frag";
    desc.layout.stride    = 20;
    desc.layout.count     = 3;
    desc.layout.attribs[0] = { 0, 0,  GpuAttribFmt::F3 };        // pos
    desc.layout.attribs[1] = { 1, 12, GpuAttribFmt::F1 };        // size
    desc.layout.attribs[2] = { 2, 16, GpuAttribFmt::U8x4_NORM }; // color

    desc.raster.topology    = GpuTopology::POINTS;
    desc.raster.blend_enable = false;
    desc.raster.depth_test  = true;
    desc.raster.depth_write = false;  // particles don't write depth
    desc.raster.cull_back   = false;  // points have no faces
    desc.raster.point_size  = true;   // GL_PROGRAM_POINT_SIZE for distance-based radius

#ifdef MD_SDL_GPU
    desc.vert_uniform_bufs = 1;  // particle.vert set=1 binding=0: viewProj + cameraPos (80B)
#endif
    pipeline_.Create(desc);

    loc_viewProj_ = pipeline_.UniformLoc("viewProj");
    loc_camPos_   = pipeline_.UniformLoc("cameraPos");

    vbuf_.Init(MAX_PARTICLES, 20);
}

void ParticleRenderer::Draw(Mat4 viewProj, Vec3 cam_pos) {
    // Build vertices from simulation SoA.
    void* ptr = vbuf_.MapWrite();
    if (!ptr) return;

    int count = ParticleSoA::Get().BuildVertices(
        static_cast<ParticleVertex*>(ptr), MAX_PARTICLES,
        cam_pos.x, cam_pos.y, cam_pos.z, PARTICLE_VIS_DIST_DEFAULT);
    vbuf_.Unmap();

    if (count <= 0) {
        vbuf_.Advance();
        return;
    }

    float cp[3] = { cam_pos.x, cam_pos.y, cam_pos.z };

    cmd_.BindPipeline(&pipeline_);
    cmd_.BindVertexBuffer(&vbuf_);
    cmd_.SetUniformMat4(loc_viewProj_, mat4_ptr(viewProj));
    cmd_.SetUniformVec3(loc_camPos_,   cp);
    cmd_.Draw((uint32_t)count);
    cmd_.EndPass();

    vbuf_.Advance();
}

void ParticleRenderer::Shutdown() {
    vbuf_.Shutdown();
    pipeline_.Destroy();
}

#ifdef MD_SDL_GPU

int ParticleRenderer::PrepareSDLGPU(md::GpuCommandBufferHandle cmd, Vec3 cam_pos) {
    void* ptr = vbuf_.MapWrite();
    if (!ptr) return 0;
    int count = ParticleSoA::Get().BuildVertices(
        static_cast<ParticleVertex*>(ptr), MAX_PARTICLES,
        cam_pos.x, cam_pos.y, cam_pos.z, PARTICLE_VIS_DIST_DEFAULT);
    vbuf_.Unmap();
    if (count > 0) vbuf_.Upload(cmd);
    return count;
}

void ParticleRenderer::DrawSDLGPU(SDL_GPURenderPass* rp, md::GpuCommandBufferHandle cmd,
                                   int count, Mat4 vp, Vec3 cam_pos) {
    if (count <= 0 || !rp || !pipeline_.SDLPipeline() || !vbuf_.SDLBuffer()) return;
    // particle.vert UBO: set=1 binding=0 — mat4 viewProj (64B) + vec3 cameraPos + pad (16B) = 80B
    struct alignas(16) PVtxUBO {
        float viewProj[16];
        float cameraPos[3]; float _pad;
    } ubo = {};
    memcpy(ubo.viewProj, mat4_ptr(vp), 64);
    ubo.cameraPos[0] = cam_pos.x;
    ubo.cameraPos[1] = cam_pos.y;
    ubo.cameraPos[2] = cam_pos.z;
    GpuPassView pv = GpuPassView::FromRaw(rp, cmd);
    pv.BindPipeline(&pipeline_);
    pv.BindVertexBuffer(&vbuf_);
    pv.PushVertexUniforms(0, &ubo, sizeof(ubo));
    pv.Draw((uint32_t)count, 1, 0, 0);
}

#endif // MD_SDL_GPU


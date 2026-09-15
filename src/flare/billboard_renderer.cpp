#include <monkey_dust/flare/billboard_renderer.h>
#include <monkey_dust/render/md_shader.h>
#include <monkey_dust/render/md_texture.h>
#include <monkey_dust/platform/math_types.h>
#include <cstring>
#include <cstdio>


#ifdef MD_SDL_GPU
#include <monkey_dust/render/gpu_device.h>
#include <monkey_dust/render/gpu_hal_types.h>
#include <monkey_dust/render/gpu_pipeline.h>
#include <monkey_dust/render/gpu_sampler_texture.h>
#include <monkey_dust/render/gpu_pass_view.h>
#include <monkey_dust/render/gpu_hal_free_functions.h>
#endif

namespace md::flare {

BillboardRenderer& BillboardRenderer::Get() {
    static BillboardRenderer inst;
    return inst;
}

// ── Counting sort helpers (shared by both backends) ───────────────────────────

static void CountingSort(const BillboardInstance* src, int count,
                         BillboardInstance* dst, int* out_starts /*[MAX_ATLAS+1]*/) {
    int tmp[BillboardRenderer::MAX_ATLAS + 1] = {};
    for (int i = 0; i < count; ++i) {
        int ai = src[i].atlas_idx < BillboardRenderer::MAX_ATLAS
               ? src[i].atlas_idx : 0;
        tmp[ai + 1]++;
    }
    for (int i = 1; i <= BillboardRenderer::MAX_ATLAS; ++i)
        tmp[i] += tmp[i - 1];
    for (int i = 0; i <= BillboardRenderer::MAX_ATLAS; ++i)
        out_starts[i] = tmp[i];
    {
        int cursor[BillboardRenderer::MAX_ATLAS] = {};
        for (int i = 0; i < count; ++i) {
            int ai = src[i].atlas_idx < BillboardRenderer::MAX_ATLAS
                   ? src[i].atlas_idx : 0;
            dst[tmp[ai] + cursor[ai]++] = src[i];
        }
    }
}

// ── Init ──────────────────────────────────────────────────────────────────────

void BillboardRenderer::Init() {
    if (init_) return;

#ifdef MD_SDL_GPU
    if (md::GpuDevice::Get().IsReady()) {
        GpuPipeline::Desc pd;
        pd.vert_path           = "shaders/billboard.vert";
        pd.frag_path           = "shaders/billboard.frag";
        pd.raster.topology     = GpuTopology::TRIANGLES;
        pd.raster.blend_enable = true;
        pd.raster.src_factor   = GpuBlendFactor::SRC_ALPHA;
        pd.raster.dst_factor   = GpuBlendFactor::ONE_MINUS_SRC_ALPHA;
        pd.raster.depth_test   = true;
        pd.raster.depth_write  = false;  // alpha sprites: test depth but don't write
        pd.raster.cull_back    = false;
        pd.vert_uniform_bufs   = 1;      // set=1: view+proj+cam_right+cam_up (160 B)
        pd.frag_uniform_bufs   = 1;      // set=3: alpha_threshold (4 B)
        pd.frag_samplers       = 1;      // set=2: one atlas per draw call
        pd.has_depth_target    = true;
        pd.layout.stride       = (uint32_t)STRIDE_SDL;
        pd.layout.count        = 5;
        pd.layout.attribs[0]   = {0,  0, GpuAttribFmt::F2};        // a_quad
        pd.layout.attribs[1]   = {1,  8, GpuAttribFmt::F3};        // a_world_pos
        pd.layout.attribs[2]   = {2, 20, GpuAttribFmt::F2};        // a_size
        pd.layout.attribs[3]   = {3, 28, GpuAttribFmt::F4};        // a_uv_rect
        pd.layout.attribs[4]   = {4, 44, GpuAttribFmt::U8x4_NORM}; // a_tint

        if (!sdl_pipeline_.Create(pd))
            fprintf(stderr, "[Billboard] SDL_GPU pipeline create failed\n");

        sdl_vbuf_.Init((uint32_t)MAX_BILLBOARDS * 6u, (uint32_t)STRIDE_SDL);

        // 1×1 transparent dummy for unused atlas binding slots.
        GpuSamplerDesc ds;
        ds.min_filter = GpuSamplerDesc::Filter::NEAREST;
        ds.mag_filter = GpuSamplerDesc::Filter::NEAREST;
        ds.gen_mipmap = false;
        ds.flip_v     = false;
        uint8_t pix[4] = {0, 0, 0, 0};
        GpuTexture dgt;
        if (dgt.InitFromMemory(pix, 1, 1, ds)) {
            sdl_dummy_tex_     = dgt.TakeSDLTexture();
            sdl_dummy_sampler_ = dgt.TakeSDLSampler();
        }
        sdl_init_ = true;
        init_     = true;
        return;
    }
#endif // MD_SDL_GPU


    init_ = true;
}

// ── Shutdown ──────────────────────────────────────────────────────────────────

void BillboardRenderer::Shutdown() {
    if (!init_) return;
    UnloadAllAtlases();

#ifdef MD_SDL_GPU
    if (sdl_init_) {
        sdl_pipeline_.Destroy();
        sdl_vbuf_.Shutdown();
        md::GpuDeviceHandle dev = md::GpuDevice::Get().SDLDevice();
        if (dev) {
            if (sdl_dummy_sampler_) GpuReleaseSampler(dev, (SDL_GPUSampler*)sdl_dummy_sampler_);
            if (sdl_dummy_tex_)     GpuReleaseTexture(dev, (md::GpuTextureHandle)sdl_dummy_tex_);
        }
        sdl_dummy_tex_ = sdl_dummy_sampler_ = nullptr;
        sdl_init_ = false;
    }
#endif


    init_ = false;
}

// ── Per-frame ─────────────────────────────────────────────────────────────────

void BillboardRenderer::BeginFrame() { count_ = 0; }

void BillboardRenderer::Submit(const BillboardInstance& inst) {
    if (count_ < MAX_BILLBOARDS) instances_[count_++] = inst;
}

int BillboardRenderer::SubmittedCount() const { return count_; }

// ── Atlas management ──────────────────────────────────────────────────────────

void BillboardRenderer::LoadSpriteAtlas(const char* png_path, int idx) {
    if (idx < 0 || idx >= MAX_ATLAS) return;
    MdUnloadTexture(atlases_[idx]);
    atlases_[idx] = MdLoadTexturePixelArt(png_path);
    bool ok = (atlases_[idx].id != 0);
#ifdef MD_SDL_GPU
    ok = ok || (atlases_[idx].sdl_tex != nullptr);
#endif
    if (!ok)
        fprintf(stderr, "[Billboard] atlas[%d] load failed: %s\n", idx, png_path);
}

void BillboardRenderer::UnloadAllAtlases() {
    for (int i = 0; i < MAX_ATLAS; ++i) MdUnloadTexture(atlases_[i]);
}

// ── OpenGL Render ─────────────────────────────────────────────────────────────

void BillboardRenderer::Render(const MdCamera& cam, float aspect) {
    (void)cam; (void)aspect;
}

// ── SDL_GPU path ──────────────────────────────────────────────────────────────

#ifdef MD_SDL_GPU

void BillboardRenderer::PrepareSDLGPU(md::GpuCommandBufferHandle cmd) {
    if (!sdl_init_ || count_ == 0) return;

    static BillboardInstance sorted[MAX_BILLBOARDS];
    CountingSort(instances_, count_, sorted, sdl_group_start_);

    // Build flat vertex buffer: 6 verts × STRIDE_SDL bytes per billboard.
    // Vertex layout: quad(8) + world_pos(12) + size(8) + uv_rect(16) + tint(4) = 48
    static const float CORNERS[6][2] = {
        {-1.f,-1.f}, {1.f,-1.f}, {1.f,1.f},
        {-1.f,-1.f}, {1.f,1.f}, {-1.f,1.f}
    };

    static uint8_t scratch[MAX_BILLBOARDS * 6 * STRIDE_SDL];
    for (int i = 0; i < count_; ++i) {
        const BillboardInstance& inst = sorted[i];
        for (int vi = 0; vi < 6; ++vi) {
            uint8_t* vp = scratch + ((size_t)i * 6 + (size_t)vi) * (size_t)STRIDE_SDL;
            float*   f  = (float*)vp;
            f[0]  = CORNERS[vi][0];  // a_quad.x
            f[1]  = CORNERS[vi][1];  // a_quad.y
            f[2]  = inst.x;          // a_world_pos.x
            f[3]  = inst.y;          // a_world_pos.y
            f[4]  = inst.z;          // a_world_pos.z
            f[5]  = inst.width;      // a_size.x
            f[6]  = inst.height;     // a_size.y
            f[7]  = inst.u0;         // a_uv_rect.x
            f[8]  = inst.v0;         // a_uv_rect.y
            f[9]  = inst.u1;         // a_uv_rect.z
            f[10] = inst.v1;         // a_uv_rect.w
            vp[44] = inst.r;
            vp[45] = inst.g;
            vp[46] = inst.b;
            vp[47] = inst.a;
        }
    }

    void* ptr = sdl_vbuf_.MapWrite();
    if (ptr) {
        memcpy(ptr, scratch, (size_t)count_ * 6u * (size_t)STRIDE_SDL);
        sdl_vbuf_.Unmap();
    }
    sdl_vbuf_.Upload(cmd);
}

void BillboardRenderer::RenderInPass(SDL_GPURenderPass* rp, md::GpuCommandBufferHandle cmd,
                                      const MdCamera& cam, float aspect) {
    if (!sdl_init_ || !rp || !cmd || count_ == 0) return;
    if (!sdl_pipeline_.SDLPipeline() || !sdl_vbuf_.SDLBuffer()) return;

    // Vertex UBO (set=1,slot=0): view+proj+cam_right+cam_up = 160 bytes std140
    struct alignas(16) BillVertUBO {
        float view[16];          // offset 0
        float proj[16];          // offset 64
        float cam_right[3]; float _p0;  // offset 128
        float cam_up[3];    float _p1;  // offset 144
    } ubo = {};

    Mat4 view = cam.ViewMatrix();
    Mat4 proj = cam.ProjMatrix(aspect);
    memcpy(ubo.view, mat4_ptr(view), 64);
    memcpy(ubo.proj, mat4_ptr(proj), 64);
    // Camera right/up from column-major view matrix: col0=right, col1=up
    const float* vf = mat4_ptr(view);
    ubo.cam_right[0] = vf[0]; ubo.cam_right[1] = vf[4]; ubo.cam_right[2] = vf[8];
    ubo.cam_up[0]    = vf[1]; ubo.cam_up[1]    = vf[5]; ubo.cam_up[2]    = vf[9];

    float alpha_thr = 0.5f;

    // GpuPassView::FromRaw (docs/HAL_CLOSURE_INVENTORY.md M1 pilot) --
    // TEMPORARY until the caller (npc_render_draw_scene.cpp) migrates its
    // own SDL_BeginGPURenderPass to GpuRenderPass; this rp/cmd pair still
    // arrives raw because THIS pass is shared across many renderers'
    // draws (one pass, not one-per-renderer), which GpuCommandBuffer's
    // owning BeginColorPass can't wrap.
    GpuPassView pv = GpuPassView::FromRaw(rp, cmd);
    pv.BindPipeline(&sdl_pipeline_);
    pv.BindVertexBuffer(&sdl_vbuf_);

    pv.PushVertexUniforms(0, &ubo, sizeof(ubo));
    pv.PushFragmentUniforms(0, &alpha_thr, sizeof(alpha_thr));

    for (int ai = 0; ai < MAX_ATLAS; ++ai) {
        int start = sdl_group_start_[ai];
        int end   = sdl_group_start_[ai + 1];
        if (start >= end) continue;

        bool has = (atlases_[ai].sdl_tex != nullptr);
        SDL_GPUTextureSamplerBinding sb = {
            has ? (md::GpuTextureHandle)atlases_[ai].sdl_tex      : (md::GpuTextureHandle)sdl_dummy_tex_,
            has ? (SDL_GPUSampler*)atlases_[ai].sdl_sampler  : (SDL_GPUSampler*)sdl_dummy_sampler_
        };
        pv.BindFragmentSamplers(0, &sb, 1);
        pv.Draw((uint32_t)(end - start) * 6u,  // vertex count
                1u,                             // instance count
                (uint32_t)start * 6u,           // first vertex
                0u);                            // first instance
    }
}

#endif // MD_SDL_GPU

} // namespace md::flare


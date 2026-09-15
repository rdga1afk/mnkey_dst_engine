// PropRenderer — per-instance rock prop draw (up to MAX_PROPS per frame, no malloc).
#include <monkey_dust/render/prop_renderer.h>
#include <monkey_dust/render/prop_tex_shared.h>
#include <monkey_dust/render/gpu_pass_view.h>
#include <cstring>
#include <cstdio>

// Declaration-only include (STB_IMAGE_IMPLEMENTATION lives in
// stb_image_impl.cpp) — needed here only for stbi_image_free() after
// uploading PropMesh::custom_tex_rgba to the GPU below.
#include "stb_image.h"

#ifdef MD_SDL_GPU
#include <SDL3/SDL_gpu.h>
#endif

bool PropRenderer::Init(const char* glb_path, float layer) {
    if (!glb_path) {
        fprintf(stdout, "[PropRenderer] No GLB path supplied — prop draw disabled\n");
        return false;
    }

    if (!mesh_.LoadGLB(glb_path, layer)) {
        fprintf(stdout, "[PropRenderer] GLB load failed — prop draw disabled\n");
        return false;
    }

    PropTexShared::Get().Init();  // idempotent; shared across all PropRenderer instances

    // task propmesh-materials (2026-08-02): upload the mesh's own embedded
    // texture (if any) as a dedicated per-instance GPU texture. Always
    // create SOME texture here (1x1 white dummy when the mesh has none) —
    // HD520 sampler binding-order rule: fragment sampler slots must be
    // bound contiguously from 0 every draw, a missing binding at slot 2
    // silently reads garbage on this hardware (see CLAUDE.md checklist).
    if (mesh_.has_custom_tex && mesh_.custom_tex_rgba) {
        GpuSamplerDesc sd;
        sd.gen_mipmap = true;
        tex_custom_.InitFromMemory(mesh_.custom_tex_rgba, mesh_.custom_tex_w,
                                    mesh_.custom_tex_h, sd);
        stbi_image_free(mesh_.custom_tex_rgba);
        mesh_.custom_tex_rgba = nullptr;  // ownership transferred to the GPU; avoid double free in PropMesh::Shutdown
    } else {
        static const uint8_t white1x1[4] = {255, 255, 255, 255};
        tex_custom_.InitFromMemory(white1x1, 1, 1, GpuSamplerDesc{});
    }

#ifdef MD_SDL_GPU
    GpuPipeline::Desc pd;
    pd.vert_path = "shaders/prop.vert";
    pd.frag_path = "shaders/prop.frag";

    // PropVertex: pos(loc=0,off=0,F3) + normal(loc=1,off=12,F3) + uv(loc=2,off=24,F2)
    // + layer(loc=3,off=32,F1), stride=36.
    pd.layout.count      = 4;
    pd.layout.stride     = 36;
    pd.layout.attribs[0] = { 0,  0, GpuAttribFmt::F3 };  // aPos
    pd.layout.attribs[1] = { 1, 12, GpuAttribFmt::F3 };  // aNormal
    pd.layout.attribs[2] = { 2, 24, GpuAttribFmt::F2 };  // aUV
    pd.layout.attribs[3] = { 3, 32, GpuAttribFmt::F1 };  // aLayer

    pd.raster.depth_test  = true;
    pd.raster.depth_write = true;
    pd.raster.cull_back   = false;  // SDL_GPU Vulkan Y-flip inverts winding vs OpenGL
    pd.has_depth_target   = true;

    pd.vert_uniform_bufs = 1;  // slot 0: PropVert UBO (80 bytes: mat4 vp + vec4 model_pos_scale)
    pd.frag_uniform_bufs = 1;  // slot 0: PropFrag UBO (32 bytes: sun_dir_str + ambient)
    pd.frag_samplers     = 3;  // 0=tex_rock, 1=tex_veg (PropTexShared), 2=tex_custom (per-instance)

    if (!pipeline_.Create(pd)) {
        fprintf(stderr, "[PropRenderer] Pipeline creation failed\n");
        mesh_.Shutdown();
        return false;
    }
#endif

    return true;
}

void PropRenderer::Shutdown() {
    pipeline_.Destroy();
    mesh_.Shutdown();
    tex_custom_.Shutdown();
}

// Vertex UBO layout (std140, 128 bytes):
//   mat4 vp              — 64 bytes
//   vec4 model_pos_scale — 16 bytes (xyz=world pos, w=scale)
//   vec4 anim_params     — 16 bytes (x=time, y=mode, z=mesh_height, w=phase)
//   vec4 model_normal    — 16 bytes (xyz=terrain normal; (0,1,0) = no tilt, G-2)
//   vec4 model_quat      — 16 bytes (xyzw; w==0 = unused, falls back to model_normal tilt)
// 128B is the documented push-constant ceiling on this project's target
// hardware (CLAUDE.md's Intel HD 520 checklist) — right at the limit, not
// over it; verified live before this became the shipped size.
struct alignas(16) PropVertUBO {
    float vp[16];              // 64 bytes
    float model_pos_scale[4];  // 16 bytes
    float anim_params[4];      // 16 bytes
    float model_normal[4];     // 16 bytes
    float model_quat[4];       // 16 bytes
};
static_assert(sizeof(PropVertUBO) == 128, "PropVertUBO size mismatch");

void PropRenderer::DrawRaw(
#ifdef MD_SDL_GPU
    SDL_GPURenderPass*    rp,
    md::GpuCommandBufferHandle cmd,
#endif
    const float* positions_xyz,
    int          count,
    const float* vp16,
    const float* sun32,
    float        scale,
    float        anim_mode,
    float        anim_time,
    const float* normals_xyz,
    const float* quats_xyzw)
{
    if (!mesh_.loaded || count <= 0) return;
    if (count > MAX_PROPS) count = MAX_PROPS;

#ifdef MD_SDL_GPU
    if (!pipeline_.SDLPipeline()) return;
    if (!mesh_.vbo.SDLBuffer() || !mesh_.ibo.SDLBuffer()) return;

    // Bind pipeline + mesh buffers once.
    GpuPassView pv = GpuPassView::FromRaw(rp, cmd);
    pv.BindPipeline(&pipeline_);
    pv.BindVertexBuffer(&mesh_.vbo);

    SDL_GPUIndexElementSize idx_size = mesh_.indices_u16
        ? SDL_GPU_INDEXELEMENTSIZE_16BIT
        : SDL_GPU_INDEXELEMENTSIZE_32BIT;
    pv.BindIndexBuffer(&mesh_.ibo, idx_size);

    // Bind rock+veg samplers (PropTexShared) + this instance's own texture
    // (tex_custom_ — real per-mesh diffuse when PropMesh::has_custom_tex,
    // else the 1x1 white dummy from Init()) — aLayer picks between all 3 in
    // prop.frag. All 3 slots always bound together (HD520: no gaps).
    PropTexShared& pt = PropTexShared::Get();
    if (pt.ready) {
        SDL_GPUTextureSamplerBinding sb[3] = {
            { pt.tex_rock->SDLTexture(),   pt.tex_rock->SDLSampler()   },
            { pt.tex_veg->SDLTexture(),    pt.tex_veg->SDLSampler()    },
            { tex_custom_.SDLTexture(),    tex_custom_.SDLSampler()    },
        };
        pv.BindFragmentSamplers(0, sb, 3);
    }

    // Push fragment UBO once (sun params are shared for all props).
    pv.PushFragmentUniforms(0, sun32, 32);

    // Per-instance draws (no malloc, fixed UBO stack).
    PropVertUBO v_ubo;
    memcpy(v_ubo.vp, vp16, 64);

    // mesh_height from AABB (y-extent in model space, before scale)
    float mesh_h = mesh_.aabb_y_max - mesh_.aabb_y_min;

    v_ubo.anim_params[0] = anim_time;
    v_ubo.anim_params[1] = anim_mode;
    v_ubo.anim_params[2] = mesh_h;

    for (int i = 0; i < count; ++i) {
        const float* p = positions_xyz + i * 3;
        v_ubo.model_pos_scale[0] = p[0];
        v_ubo.model_pos_scale[1] = p[1];
        v_ubo.model_pos_scale[2] = p[2];
        v_ubo.model_pos_scale[3] = scale;
        v_ubo.anim_params[3] = (p[0] * 0.31f + p[2] * 0.17f);
        if (normals_xyz) {
            const float* n = normals_xyz + i * 3;
            v_ubo.model_normal[0] = n[0];
            v_ubo.model_normal[1] = n[1];
            v_ubo.model_normal[2] = n[2];
        } else {
            v_ubo.model_normal[0] = 0.f;
            v_ubo.model_normal[1] = 1.f;
            v_ubo.model_normal[2] = 0.f;
        }
        v_ubo.model_normal[3] = 0.f;

        if (quats_xyzw) {
            memcpy(v_ubo.model_quat, quats_xyzw + i * 4, 16);
        } else {
            v_ubo.model_quat[0] = 0.f;
            v_ubo.model_quat[1] = 0.f;
            v_ubo.model_quat[2] = 0.f;
            v_ubo.model_quat[3] = 0.f;  // w==0 -> shader falls back to model_normal tilt
        }

        pv.PushVertexUniforms(0, &v_ubo, sizeof(v_ubo));
        pv.DrawIndexed(mesh_.index_count, 1, 0, 0, 0);
    }
#else
    (void)positions_xyz; (void)count; (void)vp16; (void)sun32;
    (void)scale; (void)anim_mode; (void)anim_time;
#endif
}

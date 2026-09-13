#pragma once
#include <monkey_dust/render/md_camera.h>
#include <monkey_dust/render/md_texture.h>
#include <cstdint>

#ifdef MD_SDL_GPU
#include <monkey_dust/render/gpu_hal.h>
#include <SDL3/SDL_gpu.h>
#endif

namespace md::flare {

struct BillboardInstance {
    float    x = 0.f, y = 0.f, z = 0.f;         // world center
    float    width = 0.f, height = 0.f;
    float    u0 = 0.f, v0 = 0.f, u1 = 0.f, v1 = 0.f;  // atlas UV rect [0,1]
    uint8_t  r = 255, g = 255, b = 255, a = 255;      // tint color
    uint8_t  atlas_idx = 0;   // which atlas slot [0..MAX_ATLAS-1]
};

constexpr int MAX_BILLBOARDS = 4096;

// Renders Flare 2D sprites as camera-facing quads in 3D world space.
// Supports up to MAX_ATLAS=4 sprite atlases per frame.
// Instances are CPU-sorted by atlas_idx; one draw call per atlas group.
//
// OpenGL per-frame usage:
//   BeginFrame() → Submit(inst) × N → Render(cam, aspect)
//
// SDL_GPU per-frame usage:
//   BeginFrame() → Submit(inst) × N → PrepareSDLGPU(cmd) → [render pass] → RenderInPass(rp, cmd, cam, aspect)
class BillboardRenderer {
public:
    static constexpr int MAX_ATLAS = 4;

    static BillboardRenderer& Get();

    void Init();
    void Shutdown();

    void BeginFrame();
    void Submit(const BillboardInstance& inst);

    // OpenGL path: standalone render (acquires nothing external).
    void Render(const MdCamera& cam, float aspect);

#ifdef MD_SDL_GPU
    // SDL_GPU path: call before the render pass begins.
    // Sorts instances, builds flat vertex buffer, uploads via copy pass.
    void PrepareSDLGPU(md::GpuCommandBufferHandle cmd);

    // SDL_GPU path: draw into an already-open render pass.
    void RenderInPass(SDL_GPURenderPass* rp, md::GpuCommandBufferHandle cmd,
                      const MdCamera& cam, float aspect);
#endif

    void LoadSpriteAtlas(const char* png_path, int idx = 0);
    void UnloadAllAtlases();
    void UnloadSpriteAtlas() { UnloadAllAtlases(); }  // backward-compat alias

    int SubmittedCount() const;
    int AtlasWidth(int idx = 0) const {
        return (idx >= 0 && idx < MAX_ATLAS) ? atlases_[idx].w : 0;
    }
    int AtlasHeight(int idx = 0) const {
        return (idx >= 0 && idx < MAX_ATLAS) ? atlases_[idx].h : 0;
    }

private:
    BillboardRenderer() = default;

    // ── OpenGL members ────────────────────────────────────────────────────────
    uint32_t vao_      = 0;
    uint32_t quad_vbo_ = 0;
    uint32_t inst_vbo_ = 0;
    uint32_t prog_     = 0;

    int loc_view_      = -1;
    int loc_proj_      = -1;
    int loc_cam_right_ = -1;
    int loc_cam_up_    = -1;
    int loc_alpha_thr_ = -1;

#ifdef MD_SDL_GPU
    // ── SDL_GPU members ───────────────────────────────────────────────────────
    // Vertex layout (stride=48, flat — no instancing):
    //   offset  0: a_quad      vec2  (8 B)
    //   offset  8: a_world_pos vec3  (12 B)
    //   offset 20: a_size      vec2  (8 B)
    //   offset 28: a_uv_rect   vec4  (16 B)
    //   offset 44: a_tint      ubyte4_norm (4 B)
    static constexpr int STRIDE_SDL = 48;

    bool            sdl_init_          = false;
    GpuPipeline     sdl_pipeline_;
    GpuVertexBuffer sdl_vbuf_;
    void*           sdl_dummy_tex_     = nullptr;  // SDL_GPUTexture* — transparent 1×1
    void*           sdl_dummy_sampler_ = nullptr;  // SDL_GPUSampler*

    // sdl_group_start_[i] = instance index where atlas group i begins (after sort).
    int sdl_group_start_[MAX_ATLAS + 1] = {};
#endif

    MdTexture atlases_[MAX_ATLAS] = {};

    BillboardInstance instances_[MAX_BILLBOARDS] = {};
    int               count_ = 0;

    bool init_ = false;
};

} // namespace md::flare

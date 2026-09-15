#pragma once
#include <monkey_dust/platform/math_types.h>
#include <monkey_dust/render/ssbo.h>
#include <monkey_dust/render/gpu_compute.h>
#include <monkey_dust/render/md_camera.h>
#include <monkey_dust/render/md_shader.h>
#include <monkey_dust/render/md_mesh.h>
#include <cmath>
#include <cstdio>
#include <cstdint>


#ifndef DEG2RAD
#define DEG2RAD 0.01745329251f
#endif

// Vector4Transform was removed from raymath in Raylib 6.x.
// In GLM mode, matrix * vector handles this directly.
#ifdef USE_GLM
static inline Vec4 ShadowVec4Transform(Vec4 v, Mat4 m) { return m * v; }
#else
static inline Vec4 ShadowVec4Transform(Vec4 v, Mat4 m) {
    return {
        m.m0*v.x + m.m4*v.y + m.m8 *v.z + m.m12*v.w,
        m.m1*v.x + m.m5*v.y + m.m9 *v.z + m.m13*v.w,
        m.m2*v.x + m.m6*v.y + m.m10*v.z + m.m14*v.w,
        m.m3*v.x + m.m7*v.y + m.m11*v.z + m.m15*v.w,
    };
}
#endif


// ─────────────────────────────────────────────────────────
// ShadowSystem — light-space matrix calculator for the directional sun's
// 4-cascade CSM split scheme (near 0-20 m, mid 20-60 m, far 60-150 m,
// ultra 150-350 m).
//
// Historically also owned the CSM depth-texture rendering path itself
// (shadow_cull.comp GPU cull -> shadow_csm.vert/frag depth-only draw,
// RenderShadowPassSDL) -- removed 2026-08-09 after discovering it was
// dead code: EvsmShadow (moment-map shadows, evsm_shadow.h) superseded
// it at some earlier point in the project's history and is the only
// shadow-casting path actually wired into NpcRender::DrawShadowMaps
// (npc_render_init.cpp), but nothing had ever deleted the superseded CSM
// machinery. Confirmed via direct grep (RenderShadowPassSDL had zero
// call sites anywhere in engine/game/tools) before removing it -- a CPU
// port of shadow_cull.comp was attempted first, then reverted once this
// came to light, since porting dead code adds no real de-risking value.
//
// What's still live and used by EvsmShadow: Update() (recomputes
// lightViewProj[4] every frame) and the lightViewProj[]/cascade_splits[]
// fields themselves.
// ─────────────────────────────────────────────────────────

class ShadowSystem {
public:
    static constexpr int   NUM_CASCADES = 4;
    static constexpr int   MAP_SIZE     = 1024;
    static constexpr float SPLITS[5]    = { 0.1f, 20.0f, 60.0f, 150.0f, 350.0f }; // VBfA-OPT-2

    static ShadowSystem& Get() {
        static ShadowSystem inst;
        return inst;
    }

    Mat4  lightViewProj[NUM_CASCADES]  = {};
    float cascade_splits[NUM_CASCADES] = { SPLITS[1], SPLITS[2], SPLITS[3], SPLITS[4] };

#ifdef MD_SDL_GPU
    // Sets sdl_init_ so Update() below actually runs. Called from
    // InitNpcSDL() in pure SDL_GPU builds. Real depth-texture/pipeline
    // creation removed 2026-08-09 along with the rest of the dead CSM
    // path -- see this class's own doc comment.
    void InitSDLGPU() {
        sdl_init_ = true;
    }
#endif

    void Init(MdMesh npc_mesh) {
    }

    // Recompute light-space VP matrices from camera frustum corners.
    // Pure CPU math — no GL calls; works in both OpenGL and SDL_GPU paths.
    void Update(const MdCamera& camera, Vec3 sun_dir, int sw, int sh) {
        if (!sdl_init_) return;
        cam_pos_  = camera.pos;
        screen_w_ = (sw > 0) ? sw : 1280;
        screen_h_ = (sh > 0) ? sh : 720;

        float aspect = (float)screen_w_ / (float)screen_h_;
        float fovY   = camera.fovy * DEG2RAD;

        Vec3 fwd   = vec3_norm(vec3_sub(camera.target, camera.pos));
        Vec3 right = vec3_norm(vec3_cross(fwd, Vec3{ 0.f, 1.f, 0.f }));
        Vec3 up_c  = vec3_cross(right, fwd);

        Vec3 to_sun = { -sun_dir.x, -sun_dir.y, -sun_dir.z };
        Vec3 lup    = (fabsf(sun_dir.y) < 0.99f) ?
                       Vec3{ 0.f, 1.f, 0.f } : Vec3{ 1.f, 0.f, 0.f };

        Vec3 cam_p = camera.pos;
        for (int k = 0; k < NUM_CASCADES; ++k) {
            float nk = SPLITS[k],  fk = SPLITS[k + 1];
            float tanH = tanf(fovY * 0.5f);
            float nH = nk * tanH, nW = nH * aspect;
            float fH = fk * tanH, fW = fH * aspect;

            Vec3 nc = vec3_add(cam_p, vec3_scale(fwd, nk));
            Vec3 fc = vec3_add(cam_p, vec3_scale(fwd, fk));

            Vec3 corners[8] = {
                vec3_add(vec3_add(nc, vec3_scale(up_c,  nH)), vec3_scale(right,  nW)),
                vec3_add(vec3_add(nc, vec3_scale(up_c,  nH)), vec3_scale(right, -nW)),
                vec3_add(vec3_add(nc, vec3_scale(up_c, -nH)), vec3_scale(right,  nW)),
                vec3_add(vec3_add(nc, vec3_scale(up_c, -nH)), vec3_scale(right, -nW)),
                vec3_add(vec3_add(fc, vec3_scale(up_c,  fH)), vec3_scale(right,  fW)),
                vec3_add(vec3_add(fc, vec3_scale(up_c,  fH)), vec3_scale(right, -fW)),
                vec3_add(vec3_add(fc, vec3_scale(up_c, -fH)), vec3_scale(right,  fW)),
                vec3_add(vec3_add(fc, vec3_scale(up_c, -fH)), vec3_scale(right, -fW)),
            };

            Vec3 center = {};
            for (int i = 0; i < 8; ++i)
                center = vec3_add(center, vec3_scale(corners[i], 0.125f));

            Mat4 lv = mat4_lookat(vec3_add(center, to_sun), center, lup);

            float minX = 1e9f, maxX = -1e9f;
            float minY = 1e9f, maxY = -1e9f;
            float minZ = 1e9f, maxZ = -1e9f;
            for (int i = 0; i < 8; ++i) {
                Vec4 lc = ShadowVec4Transform(
                    { corners[i].x, corners[i].y, corners[i].z, 1.f }, lv);
                if (lc.x < minX) minX = lc.x; if (lc.x > maxX) maxX = lc.x;
                if (lc.y < minY) minY = lc.y; if (lc.y > maxY) maxY = lc.y;
                if (lc.z < minZ) minZ = lc.z; if (lc.z > maxZ) maxZ = lc.z;
            }
            minZ -= 50.0f; // extend backward to catch casters behind the slice

            // Texel-snap: quantize the light-space frustum center to the nearest
            // shadow-map texel so the shadow map moves in whole-texel steps.
            // Eliminates shadow shimmer (sub-texel jitter) during camera movement.
            {
                float texelX = (maxX - minX) / MAP_SIZE;
                float texelY = (maxY - minY) / MAP_SIZE;
                float cx = (minX + maxX) * 0.5f;
                float cy = (minY + maxY) * 0.5f;
                cx = roundf(cx / texelX) * texelX;
                cy = roundf(cy / texelY) * texelY;
                float rx = (maxX - minX) * 0.5f;
                float ry = (maxY - minY) * 0.5f;
                minX = cx - rx;  maxX = cx + rx;
                minY = cy - ry;  maxY = cy + ry;
            }

            Mat4 lp = mat4_ortho(minX, maxX, minY, maxY, minZ, maxZ);
#ifdef MD_SDL_GPU
            lightViewProj[k] = mat4_mul(lp, lv);
#else
            lightViewProj[k] = mat4_mul(lv, lp);
#endif
        }
    }

    // Shadow cull + depth render.
    // Caller must pre-bind: transform SSBO at 0, finalBones SSBO at 4.
    // SDL_GPU: pass bindings.cmd; rw[0]=shadow_vis, rw[1]=shadow_ind, ro[0]=transform.
    //          bindings.cmd=nullptr is a safe no-op (Step 9 wires SDL_GPUBuffer*).
    void RenderShadowPass(int active_npc_count,
                          const GpuComputePass::StorageBindings& bindings = {}) {
    }

    // Binds shadow maps to texture units 5/6/7 and sets CSM uniforms.
    // Caller must have the npc shader active (glUseProgram / MdUseShader).
    void BindShadowMaps(MdShader shader) {
    }

    void Shutdown() {
    }

private:
    ShadowSystem() = default;

    bool               init_         = false;
#ifdef MD_SDL_GPU
    bool               sdl_init_     = false;
#endif
    MdShader           shadow_shader_;
    MdMesh        mesh_;
    int           mesh_idx_ = 0;
    Vec3          cam_pos_ = {};
    int  loc_lightVP_  = -1;
    int  loc_svCamPos_ = -1, loc_svMaxDist_ = -1, loc_svTotal_ = -1;
    int  screen_w_ = 1280, screen_h_ = 720;
};

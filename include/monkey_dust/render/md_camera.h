#pragma once
// Portable camera abstraction — replaces Camera3D in rendering code (M6+).
// Rule M-C: use Vec3/Mat4 (math_types.h), not Vector3/Matrix directly.
//
// ViewMatrix()/ProjMatrix(): platform-neutral via mat4_lookat/mat4_perspective.
// The Raylib adapters (ToRaylib/FromRaylib/GetViewProjRaylib) were removed
// 2026-08-09 along with the rest of the Raylib fallback skeleton -- raylib.h
// is never included anymore (USE_SDL3=ON is the only buildable path), so
// RAYLIB_H was never defined and that code was provably dead.

#include <monkey_dust/platform/math_types.h>

// #156 FSR 2: the sub-pixel jitter of the projection of the frame being rendered, in NDC (x right, y up as clip space; 0 = none). Set once per frame by the renderer
// (game/src/render/npc_render_deferred.cpp) while FSR 2 is active; every ProjMatrix() of that frame carries it. MotionVectorProj() is the projection without it.
inline float g_md_proj_jitter_ndc[2] = { 0.f, 0.f };

struct MdCamera {
    Vec3  pos;
    Vec3  target;
    Vec3  up;
    float fovy;   // degrees

    Mat4 ViewMatrix() const { return mat4_lookat(pos, target, up); }
    // Correct VP product for the active backend.
    // SDL_GPU (column-major, depth [0,1]): clip = Proj * View * pos → P * V.
    // Raylib/OpenGL (row-major convention): V * P was correct historically.
    Mat4 ViewProjMatrix(float aspect) const {
#ifdef MD_SDL_GPU
        return mat4_mul(ProjMatrix(aspect), ViewMatrix());
#else
        return mat4_mul(ViewMatrix(), ProjMatrix(aspect));
#endif
    }

    Mat4 ProjMatrixNoJitter(float aspect) const {
        static constexpr float DEG2R = 0.01745329251f;
        return mat4_perspective(fovy * DEG2R, aspect, 0.5f, 4000.f);
    }
    Mat4 ProjMatrix(float aspect) const {
        Mat4 P = ProjMatrixNoJitter(aspect);
        if (g_md_proj_jitter_ndc[0] != 0.f || g_md_proj_jitter_ndc[1] != 0.f) {
            float* m = const_cast<float*>(mat4_ptr(P));   // column-major, clip.w = -z_view: ndc += j needs m[8] / m[9] (column 2, rows 0 / 1) -= j
            m[8] -= g_md_proj_jitter_ndc[0]; m[9] -= g_md_proj_jitter_ndc[1];
        }
        return P;
    }

    inline void CamPosToArr(float out[3]) const {
        out[0] = pos.x; out[1] = pos.y; out[2] = pos.z;
    }

    // Portable frustum plane extraction — works in any build.
    // Fills 4 planes (left, right, top, bottom) as vec4[4] for the cull shader.
    inline void FrustumPlanes(float aspect, float out[16]) const {
        Mat4 vp = ViewProjMatrix(aspect);
        const float* m = mat4_ptr(vp);
        // Gribb & Hartmann: row3 ± rowN from column-major float[16].
        out[ 0]=m[3]+m[0]; out[ 1]=m[7]+m[4]; out[ 2]=m[11]+m[ 8]; out[ 3]=m[15]+m[12]; // left
        out[ 4]=m[3]-m[0]; out[ 5]=m[7]-m[4]; out[ 6]=m[11]-m[ 8]; out[ 7]=m[15]-m[12]; // right
        out[ 8]=m[3]-m[1]; out[ 9]=m[7]-m[5]; out[10]=m[11]-m[ 9]; out[11]=m[15]-m[13]; // top
        out[12]=m[3]+m[1]; out[13]=m[7]+m[5]; out[14]=m[11]+m[ 9]; out[15]=m[15]+m[13]; // bottom
    }
};

// Project a world-space point to screen pixels (column-major VP, Y-down screen).
// Portable: uses mat4_ptr — no Raylib types required.
inline Vec2 MdWorldToScreen(Vec3 world, const MdCamera& cam, int sw, int sh) {
    float aspect = (sw > 0 && sh > 0) ? (float)sw / (float)sh : 1.f;
    Mat4         vp = cam.ViewProjMatrix(aspect);
    const float*  m = mat4_ptr(vp);
    float cx = m[0]*world.x + m[4]*world.y + m[8] *world.z + m[12];
    float cy = m[1]*world.x + m[5]*world.y + m[9] *world.z + m[13];
    float cw = m[3]*world.x + m[7]*world.y + m[11]*world.z + m[15];
    if (fabsf(cw) < 1e-6f) return {};
    return { (cx/cw + 1.f) * 0.5f * (float)sw,
             (1.f - cy/cw) * 0.5f * (float)sh };
}

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
#include <monkey_dust/render/kenshi_sky.h>

struct MdCamera {
    Vec3  pos;
    Vec3  target;
    Vec3  up;
    float fovy;   // degrees
    // Exact view direction (not normalised). Set by MdCameraTranslated: a camera moved by ~1e4 m must not get its direction back as target - pos of two big floats (1 mm ulp over a ~1 m vector = ~1 px).
    Vec3  dir     = { 0.f, 0.f, 0.f };
    bool  has_dir = false;

    Mat4 ViewMatrix() const {
        if (!has_dir) return mat4_lookat(pos, target, up);
        return mat4_mul(mat4_lookat(Vec3{ 0.f, 0.f, 0.f }, dir, up), mat4_translate(-pos.x, -pos.y, -pos.z));   // rotation from the direction alone, the position only enters the translation
    }
    // Correct VP product for the active backend.
    // SDL_GPU (column-major, depth [0,1]): clip = Proj * View * pos → P * V.
    // Raylib/OpenGL (row-major convention): V * P was correct historically.
    Mat4 ViewProjMatrix(float aspect) const {
        return mat4_mul(ProjMatrix(aspect), ViewMatrix());
    }

    Mat4 ProjMatrix(float aspect) const {
        static constexpr float DEG2R = 0.01745329251f;
        return mat4_perspective(fovy * DEG2R, aspect, 0.5f, md::ViewFarClipM());
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

// The same camera moved by (dx, 0, dz): the view direction is taken from the camera's own (small, local) numbers, never from the difference of the moved position and target.
inline MdCamera MdCameraTranslated(const MdCamera& cam, float dx, float dz) {
    MdCamera c = cam;
    const Vec3 d = cam.has_dir ? cam.dir : Vec3{ cam.target.x - cam.pos.x, cam.target.y - cam.pos.y, cam.target.z - cam.pos.z };
    c.pos.x += dx; c.pos.z += dz;
    c.target = { c.pos.x + d.x, c.pos.y + d.y, c.pos.z + d.z };   // kept consistent for code that reads target; the view matrix uses dir
    c.dir = d; c.has_dir = true;
    return c;
}

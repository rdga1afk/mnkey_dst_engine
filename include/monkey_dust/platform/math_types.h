#pragma once

// Portable math type aliases.
// Default: Raylib types (no dependency change for existing code).
// Migration: compile with -DUSE_GLM to switch to glm — one flag, zero code changes.
// Rule M-C: use Vec3/Mat4 (not Vector3/Matrix) in all new files from Phase 22+.

#ifndef USE_GLM
#  include "raylib.h"
#  include "raymath.h"
   using Vec2 = Vector2;
   using Vec3 = Vector3;
   using Vec4 = Vector4;
   using Mat4 = Matrix;
   using Quat = Quaternion;
   inline Vec3 vec3_add  (Vec3 a, Vec3 b)    { return Vector3Add(a, b); }
   inline Vec3 vec3_sub  (Vec3 a, Vec3 b)    { return Vector3Subtract(a, b); }
   inline Vec3 vec3_scale(Vec3 v, float s)   { return Vector3Scale(v, s); }
   inline Vec3 vec3_cross(Vec3 a, Vec3 b)    { return Vector3CrossProduct(a, b); }
   inline float vec3_dot (Vec3 a, Vec3 b)    { return Vector3DotProduct(a, b); }
   inline float vec3_len (Vec3 v)            { return Vector3Length(v); }
   inline Vec3 vec3_norm (Vec3 v)            { return Vector3Normalize(v); }
   inline Mat4 mat4_identity()               { return MatrixIdentity(); }
   inline Mat4 mat4_mul  (Mat4 a, Mat4 b)    { return MatrixMultiply(a, b); }
   inline Mat4 mat4_translate(float x, float y, float z) { return MatrixTranslate(x, y, z); }
   inline Mat4 mat4_scale(float x, float y, float z)     { return MatrixScale(x, y, z); }
   inline Mat4 mat4_rotate_y(float angle)    { return MatrixRotateY(angle); }
   inline Mat4 mat4_perspective(float fovy, float aspect, float near_z, float far_z)
                                             { if (!(aspect > 0.001f)) aspect = 16.f/9.f;
                                               return MatrixPerspective(fovy, aspect, near_z, far_z); }
   inline Mat4 mat4_lookat(Vec3 eye, Vec3 center, Vec3 up) { return MatrixLookAt(eye, center, up); }
   inline Mat4 mat4_ortho(float l, float r, float b, float t, float n, float f)
                                             { return MatrixOrtho(l, r, b, t, n, f); }
   inline const float* mat4_ptr(const Mat4& m) { return &m.m0; }
   inline Mat4 mat4_inv(Mat4 m)               { return MatrixInvert(m); }
#else
// SDL_GPU/Vulkan clip space: depth [0,1], Y points down.
// GLM_FORCE_DEPTH_ZERO_TO_ONE makes perspective() generate depth in [0,1].
#    define GLM_FORCE_DEPTH_ZERO_TO_ONE
#  include <glm/glm.hpp>
#  include <glm/gtc/matrix_transform.hpp>
#  include <glm/gtc/type_ptr.hpp>
   using Vec2 = glm::vec2;
   using Vec3 = glm::vec3;
   using Vec4 = glm::vec4;
   using Mat4 = glm::mat4;
   using Quat = glm::quat;
   inline Vec3 vec3_add  (Vec3 a, Vec3 b)    { return a + b; }
   inline Vec3 vec3_sub  (Vec3 a, Vec3 b)    { return a - b; }
   inline Vec3 vec3_scale(Vec3 v, float s)   { return v * s; }
   inline Vec3 vec3_cross(Vec3 a, Vec3 b)    { return glm::cross(a, b); }
   inline float vec3_dot (Vec3 a, Vec3 b)    { return glm::dot(a, b); }
   inline float vec3_len (Vec3 v)            { return glm::length(v); }
   inline Vec3 vec3_norm (Vec3 v)            { return glm::normalize(v); }
   inline Mat4 mat4_identity()               { return glm::mat4(1.0f); }
   inline Mat4 mat4_mul  (Mat4 a, Mat4 b)    { return a * b; }
   inline Mat4 mat4_translate(float x, float y, float z)
                                             { return glm::translate(glm::mat4(1.0f), {x, y, z}); }
   inline Mat4 mat4_scale(float x, float y, float z)
                                             { return glm::scale(glm::mat4(1.0f), {x, y, z}); }
   inline Mat4 mat4_rotate_y(float angle)    { return glm::rotate(glm::mat4(1.0f), angle, {0.0f, 1.0f, 0.0f}); }
   inline Mat4 mat4_perspective(float fovy, float aspect, float near_z, float far_z)
                                             { if (!(aspect > 0.001f)) aspect = 16.f/9.f;
                                               return glm::perspective(fovy, aspect, near_z, far_z); }
   inline Mat4 mat4_lookat(Vec3 eye, Vec3 center, Vec3 up) { return glm::lookAt(eye, center, up); }
   inline Mat4 mat4_ortho(float l, float r, float b, float t, float n, float f)
                                             { return glm::ortho(l, r, b, t, n, f); }
   inline const float* mat4_ptr(const Mat4& m) { return glm::value_ptr(m); }
   inline Mat4 mat4_inv(Mat4 m)               { return glm::inverse(m); }
#endif

// Simple ray: origin + normalised direction.  Defined after Vec3 aliases.
struct MdRay { Vec3 pos; Vec3 dir; };

// ── Hot math primitives (RE: lerp score=287/85 call-ins, inv_lerp score=99) ──
// Both confirmed as the single hottest math functions in Kenshi engine binary.
// MD_FORCE_INLINE guarantees no call overhead — critical for animation/physics hot-paths.
#include <monkey_dust/platform/md_hints.h>
#include <cmath>

MD_FORCE_INLINE float float_lerp(float a, float b, float t) noexcept {
    return a + (b - a) * t;
}
MD_FORCE_INLINE float float_inv_lerp(float a, float b, float v) noexcept {
    return (v - a) / (b - a);
}
MD_FORCE_INLINE float float_clamp(float v, float lo, float hi) noexcept {
    return v < lo ? lo : v > hi ? hi : v;
}
MD_FORCE_INLINE float float_clamp01(float v) noexcept {
    return v < 0.f ? 0.f : v > 1.f ? 1.f : v;
}
MD_FORCE_INLINE Vec3 vec3_lerp(Vec3 a, Vec3 b, float t) noexcept {
    return { float_lerp(a.x, b.x, t), float_lerp(a.y, b.y, t), float_lerp(a.z, b.z, t) };
}
// Normalised lerp — cheaper than slerp, good enough for most bone blends.
MD_FORCE_INLINE Quat quat_nlerp(Quat a, Quat b, float t) noexcept {
    float bx = b.x, by = b.y, bz = b.z, bw = b.w;
    // Ensure shortest arc (dot < 0 → negate b)
    if (a.x*bx + a.y*by + a.z*bz + a.w*bw < 0.f) { bx=-bx; by=-by; bz=-bz; bw=-bw; }
    float rx = float_lerp(a.x, bx, t);
    float ry = float_lerp(a.y, by, t);
    float rz = float_lerp(a.z, bz, t);
    float rw = float_lerp(a.w, bw, t);
    float inv = 1.f / sqrtf(rx*rx + ry*ry + rz*rz + rw*rw);
    return { rx*inv, ry*inv, rz*inv, rw*inv };
}

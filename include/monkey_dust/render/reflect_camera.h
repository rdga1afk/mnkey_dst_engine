// Planar reflection camera math, in double, column-major, depth [0, 1] (our projection, md_camera.h).
// ReflectionMatrix: OGRE 1.9 Math::buildReflectionMatrix (OgreMath.cpp, plane normal . p + d = 0); the reflected view is View * R (OgreMath.cpp makeViewMatrix).
// ObliqueNearClip: the near plane replaced by a plane in view space (Lengyel, as OgreFrustum.cpp updateFrustumImpl does for GL depth; for depth [0, 1] row 2 = C / (C . Q)).
#pragma once
#include <cmath>

namespace md::reflect {

// Plane: unit normal n, offset d (n . p + d = 0). Out: 4x4 column-major, p' = R p mirrors p about the plane.
inline void ReflectionMatrix(const double n[3], double d, double out[16]) {
    for (int c = 0; c < 4; ++c) for (int r = 0; r < 4; ++r) out[c * 4 + r] = 0.0;
    for (int r = 0; r < 3; ++r) { for (int c = 0; c < 3; ++c) out[c * 4 + r] = (r == c ? 1.0 : 0.0) - 2.0 * n[r] * n[c]; out[12 + r] = -2.0 * n[r] * d; }
    out[15] = 1.0;
}

// proj: column-major, clip = proj * view_pos. plane (nx, ny, nz, d) in VIEW space. Out: proj with row 2 replaced so that clip z = 0 on the plane.
// The kept half-space is the positive one (the camera on it: d > 0) and the far top / side corner of the frustum must lie on it too, else the plane cuts all the distance away.
inline void ObliqueNearClip(const double proj[16], const double plane[4], double out[16]) {
    for (int i = 0; i < 16; ++i) out[i] = proj[i];
    auto P = [&](int r, int c) { return proj[c * 4 + r]; };
    // Q: the view-space point (w = 1) whose clip coordinates are (sgn(Cx), sgn(Cy), 1, 1) times lambda, i.e. the far corner opposite the plane (Lengyel)
    const double sx = plane[0] > 0 ? 1.0 : (plane[0] < 0 ? -1.0 : 0.0), sy = plane[1] > 0 ? 1.0 : (plane[1] < 0 ? -1.0 : 0.0);
    const double qz = (P(3, 3) - P(2, 3)) / (P(2, 2) - P(3, 2));            // z' = w' fixes the depth
    const double lambda = P(3, 2) * qz + P(3, 3);
    const double qx = (sx * lambda - P(0, 2) * qz - P(0, 3)) / P(0, 0);
    const double qy = (sy * lambda - P(1, 2) * qz - P(1, 3)) / P(1, 1);
    const double dot = plane[0] * qx + plane[1] * qy + plane[2] * qz + plane[3];
    const double k = lambda / dot;                                           // z' = lambda at Q, z' = 0 on the plane
    for (int c = 0; c < 4; ++c) out[c * 4 + 2] = plane[c] * k;
}

} // namespace md::reflect

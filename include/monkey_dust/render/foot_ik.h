#pragma once
// FootIK (M59) — analytical 2-bone foot placement IK.
//
// Kenshi Biped bone indices (from md_human.glb):
//   L leg: Thigh=2, Calf=3, Foot=4
//   R leg: Thigh=7, Calf=8, Foot=9
//
// Usage per NPC (Tier1 LOD only — < 30m from camera):
//   1. Call SkinMesh::GetFinalBonesFull(clip, t, bones, model_xyz, world_mats)
//   2. FootIK::SolveLeg(model_xyz, world_mats, hip, knee, ankle,
//                        npc_y, target_y, out_hip16, out_knee16, out_ankle16)
//   3. SkinMesh::PatchBoneIK(hip,   out_hip16,   bones)
//      SkinMesh::PatchBoneIK(knee,  out_knee16,  bones)
//      SkinMesh::PatchBoneIK(ankle, out_ankle16, bones)
//
// Algorithm: law-of-cosines + pole-vector. Explicitly computes new knee world
// position so child bones don't splay. Inputs are TRUE world matrices from
// GetFinalBonesFull (out_world_mats param) — NOT skinning matrices.

#include <cmath>
#include <cstring>

namespace FootIK {

static constexpr int L_THIGH = 2, L_CALF = 3, L_FOOT = 4;
static constexpr int R_THIGH = 7, R_CALF = 8, R_FOOT = 9;
static constexpr float MAX_FOOT_DY = 0.25f;

static inline void rot_axis_angle(float* r, float ax, float ay, float az, float angle) {
    float c = cosf(angle), s = sinf(angle), t = 1.f - c;
    r[0]=t*ax*ax+c;      r[3]=t*ax*ay-s*az; r[6]=t*ax*az+s*ay;
    r[1]=t*ax*ay+s*az;   r[4]=t*ay*ay+c;    r[7]=t*ay*az-s*ax;
    r[2]=t*ax*az-s*ay;   r[5]=t*ay*az+s*ax; r[8]=t*az*az+c;
}

// Apply rotation to the 3×3 part of a col-major mat4; translation unchanged.
static inline void apply_rot(const float* src16, const float* rot9, float* dst16) {
    memcpy(dst16, src16, 64);
    for (int col = 0; col < 3; ++col) {
        float x = src16[col*4+0], y = src16[col*4+1], z = src16[col*4+2];
        dst16[col*4+0] = rot9[0]*x + rot9[3]*y + rot9[6]*z;
        dst16[col*4+1] = rot9[1]*x + rot9[4]*y + rot9[7]*z;
        dst16[col*4+2] = rot9[2]*x + rot9[5]*y + rot9[8]*z;
    }
}

// Build a rotation matrix that rotates unit vector 'from' onto unit vector 'to'.
// out_rot is col-major 3×3. Returns false if angle is negligible.
static inline bool rot_from_to(float* out_rot,
                                float fx, float fy, float fz,
                                float tx, float ty, float tz) {
    float rax = fy*tz - fz*ty;
    float ray = fz*tx - fx*tz;
    float raz = fx*ty - fy*tx;
    float rlen = sqrtf(rax*rax + ray*ray + raz*raz);
    if (rlen < 1e-6f) {
        out_rot[0]=out_rot[4]=out_rot[8]=1.f;
        out_rot[1]=out_rot[2]=out_rot[3]=out_rot[5]=out_rot[6]=out_rot[7]=0.f;
        return false;
    }
    float dot = fx*tx + fy*ty + fz*tz;
    dot = (dot > 1.f ? 1.f : dot < -1.f ? -1.f : dot);
    rot_axis_angle(out_rot, rax/rlen, ray/rlen, raz/rlen, acosf(dot));
    return true;
}

static inline bool SolveLeg(
    const float* model_xyz,    // float[MAX_SKIN_BONES*3] from GetFinalBonesFull
    const float* world_mats,   // float[MAX_SKIN_BONES*16] — TRUE world mats, not skinning
    int hip_idx, int knee_idx, int ankle_idx,
    float npc_world_y,
    float target_world_y,
    float* out_hip16,
    float* out_knee16,
    float* out_ankle16)
{
    // Model-space joint positions
    float hx = model_xyz[hip_idx*3+0],   hy = model_xyz[hip_idx*3+1],   hz = model_xyz[hip_idx*3+2];
    float kx = model_xyz[knee_idx*3+0],  ky = model_xyz[knee_idx*3+1],  kz = model_xyz[knee_idx*3+2];
    float ax = model_xyz[ankle_idx*3+0], ay = model_xyz[ankle_idx*3+1], az = model_xyz[ankle_idx*3+2];

    float target_y = target_world_y - npc_world_y;
    float dy = target_y - ay;
    if (fabsf(dy) < 0.002f) return false;
    if (dy >  MAX_FOOT_DY) dy =  MAX_FOOT_DY;
    if (dy < -MAX_FOOT_DY) dy = -MAX_FOOT_DY;

    float tax = ax, tay = ay + dy, taz = az;

    float upper = sqrtf((kx-hx)*(kx-hx) + (ky-hy)*(ky-hy) + (kz-hz)*(kz-hz));
    float lower = sqrtf((ax-kx)*(ax-kx) + (ay-ky)*(ay-ky) + (az-kz)*(az-kz));
    if (upper < 1e-4f || lower < 1e-4f) return false;

    // Hip→target ankle
    float dx = tax-hx, dhy = tay-hy, dz = taz-hz;
    float dist = sqrtf(dx*dx + dhy*dhy + dz*dz);
    float max_r = upper + lower - 1e-4f;
    if (dist > max_r) { float s = max_r/dist; dx*=s; dhy*=s; dz*=s; dist = max_r; }
    if (dist < 1e-6f) return false;
    float d_x = dx/dist, d_y = dhy/dist, d_z = dz/dist;

    // Law of cosines → angle at hip
    float cos_h = (dist*dist + upper*upper - lower*lower) / (2.f*dist*upper);
    if (cos_h >  1.f) cos_h =  1.f;
    if (cos_h < -1.f) cos_h = -1.f;
    float sin_h = sqrtf(1.f - cos_h*cos_h);

    // Pole vector: current knee projected perpendicular to hip→ankle axis
    float kh_x = kx-hx, kh_y = ky-hy, kh_z = kz-hz;
    float proj = kh_x*d_x + kh_y*d_y + kh_z*d_z;
    float p_x = kh_x - proj*d_x, p_y = kh_y - proj*d_y, p_z = kh_z - proj*d_z;
    float plen = sqrtf(p_x*p_x + p_y*p_y + p_z*p_z);
    if (plen < 1e-6f) {
        // Straight leg — pole vector is degenerate, no reliable knee direction.
        // Any fallback axis produces visually wrong rotation (ankle twists sideways).
        // With tr.y tracking terrain, straight-leg bind pose is already correct; skip IK.
        return false;
    }
    p_x /= plen; p_y /= plen; p_z /= plen;

    // New knee world position
    float nkx = hx + d_x*upper*cos_h + p_x*upper*sin_h;
    float nky = hy + d_y*upper*cos_h + p_y*upper*sin_h;
    float nkz = hz + d_z*upper*cos_h + p_z*upper*sin_h;

    float rot[9];

    // Hip: rotate old thigh dir → new thigh dir
    float ot_x = kx-hx, ot_y = ky-hy, ot_z = kz-hz;
    float nt_x = nkx-hx, nt_y = nky-hy, nt_z = nkz-hz;
    float ot_l = sqrtf(ot_x*ot_x+ot_y*ot_y+ot_z*ot_z);
    float nt_l = sqrtf(nt_x*nt_x+nt_y*nt_y+nt_z*nt_z);
    if (ot_l < 1e-6f || nt_l < 1e-6f) return false;
    rot_from_to(rot, ot_x/ot_l, ot_y/ot_l, ot_z/ot_l,
                     nt_x/nt_l, nt_y/nt_l, nt_z/nt_l);
    apply_rot(world_mats + hip_idx*16, rot, out_hip16);
    // Hip position unchanged
    out_hip16[12] = world_mats[hip_idx*16+12];
    out_hip16[13] = world_mats[hip_idx*16+13];
    out_hip16[14] = world_mats[hip_idx*16+14];

    // Knee: rotate calf from old direction → new direction.
    // rot_from_to is rig-convention independent — no explicit hinge axis needed.
    float oc_x = ax-kx, oc_y = ay-ky, oc_z = az-kz;
    float oc_l = sqrtf(oc_x*oc_x+oc_y*oc_y+oc_z*oc_z);
    if (oc_l < 1e-6f) return false;
    float oc_nx = oc_x/oc_l, oc_ny = oc_y/oc_l, oc_nz = oc_z/oc_l;
    float nc_x = tax-nkx, nc_y = tay-nky, nc_z = taz-nkz;
    float nc_l = sqrtf(nc_x*nc_x+nc_y*nc_y+nc_z*nc_z);
    if (nc_l < 1e-6f) return false;
    rot_from_to(rot, oc_nx, oc_ny, oc_nz, nc_x/nc_l, nc_y/nc_l, nc_z/nc_l);
    apply_rot(world_mats + knee_idx*16, rot, out_knee16);
    out_knee16[12] = nkx;
    out_knee16[13] = nky;
    out_knee16[14] = nkz;

    // Ankle: keep orientation, move to target position
    memcpy(out_ankle16, world_mats + ankle_idx*16, 64);
    out_ankle16[12] = tax;
    out_ankle16[13] = tay;
    out_ankle16[14] = taz;

    return true;
}

} // namespace FootIK

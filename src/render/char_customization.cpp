// char_customization.cpp — Variant A: Kenshi bone-scale + face morph wiring.
// Port of SetBoneScalesFromDef() + kFaceMorphMap from editor_char_preview_sdlgpu.h.
// No game/ or tools/ includes. No malloc/new. No C++20. No assert().
#include <monkey_dust/render/char_customization.h>
#include <monkey_dust/render/skin_mesh.h>
#include <cstring>

// ── CharCustomization_ComputeScales ──────────────────────────────────────────
// Direct port of SetBoneScalesFromDef() bone formulas.
// All bone indices are confirmed from md_human.glb (30 bones):
//   0=Bip01(root)  1=Pelvis
//   2=L Thigh  3=L Calf  4=L Foot  5=L Toe0  6=L Toe0Nub
//   7=R Thigh  8=R Calf  9=R Foot 10=R Toe0 11=R Toe0Nub
//  12=Spine  13=Spine1  14=Spine2
//  15=L Clavicle 16=L UpperArm 17=L Forearm 18=L Hand  19=Prop1
//  20=Neck   21=Head    22=HeadNub 23=Jaw    24=JawNub
//  25=R Clavicle 26=R UpperArm 27=R Forearm 28=R Hand  29=Prop2

void CharCustomization_ComputeScales(const float body[CHARCC_BODY_N],
                                      const float face[CHARCC_FACE_N],
                                      int         bone_count,
                                      CharScales& out)
{
    // Initialise to identity
    for (int i = 0; i < MAX_SKIN_BONES; ++i) {
        out.bone[i][0] = 1.f; out.bone[i][1] = 1.f; out.bone[i][2] = 1.f;
        out.pos [i][0] = 1.f; out.pos [i][1] = 1.f; out.pos [i][2] = 1.f;
        out.rot [i]    = 0.f;
        out.qrot_delta[i][0] = 0.f; out.qrot_delta[i][1] = 0.f;
        out.qrot_delta[i][2] = 0.f; out.qrot_delta[i][3] = 1.f;
    }
    out.amputee_mask = 0;

    if (bone_count <= 0) return;

    // ── Helper lambdas ────────────────────────────────────────────────────────
    auto cl = [](float x) -> float {
        return x < 0.1f ? 0.1f : (x > 4.f ? 4.f : x);
    };
    // comp: blended scale: 1.0 + (x-1.0)*k (compresses deviation by factor k)
    auto comp = [&](float x, float k) -> float {
        return cl(1.f + (x - 1.f) * k);
    };
    // setBS: set bone vertex scales
    auto setBS = [&](int i, float wy, float wx, float wz) {
        if (i < 0 || i >= MAX_SKIN_BONES) return;
        out.bone[i][0] = wy; out.bone[i][1] = wx; out.bone[i][2] = wz;
    };

    // ── Body slider values → normalised scale factors ─────────────────────────
    // slider=100 → factor=1.0 → no deformation (mesh baked at slider=100).
    float H   = cl(body[2]  / 100.f);  // Height
    float Fr  = cl(body[3]  / 100.f);  // Frame
    float Pt  = body[4] / 35.f;        // Posture: 1.0 at neutral=35; 0=hunch, 2=erect
    float SS  = cl(body[5]  /  67.5f); // Shoulder set — neutral=67.5
    float NP  = cl(body[6]  /  52.5f); // Neck position — neutral=52.5
    float LL  = cl(body[7]  / 100.f);  // Leg length
    float Sh  = cl(body[8]  / 100.f);  // Shoulders
    float Ab  = cl(body[9]  / 100.f);  // Arm bulk
    float Wa  = cl(body[10] / 100.f);  // Waist
    float Hn  = cl(body[11] / 100.f);  // Hands
    float Ch  = cl(body[12] / 100.f);  // Chest
    float St  = cl(body[13] / 100.f);  // Stomach
    float Hips= cl(body[15] / 100.f);  // Hips
    float LgB = cl(body[16] / 100.f);  // Legs bulk
    float Ft  = cl(body[17] / 100.f);  // Feet
    float LgS = 1.0f;  // Legs shape: neutral (slider not in BODY_N=18 array)

    // ── Lower body ────────────────────────────────────────────────────────────
    // (the "Hips" slider, NOT Height — Hips=1 neutral → legXZ width
    // stays constant across Height, avoiding a compounded width+length shrink/grow.)
    float legXZ    = cl((LgS * LgB + (Hips - 1.f) / 3.f) * Fr);
    float overall_XZ = legXZ;  // used by thighs/calves
    float leg_Y = cl(H + LL - 1.f);

    setBS(1, H, comp(Hips,0.6f)*Fr, comp(Hips,0.6f)*Fr);

    // Thighs [2,7]: setBoneSize + setBonePositionalSize Y (lateral)
    setBS(2, leg_Y * 0.95f, overall_XZ, overall_XZ);
    setBS(7, leg_Y * 0.95f, overall_XZ, overall_XZ);
    // bind_t[2/7][1] (~0.099) is the LATERAL hip-spread offset, not the length axis —
    // scaling it by an H-dependent factor pushed legs apart at low H ("balloon pants").
    // RE's setBonePositionalSize Y-component maps to bind_t[][0] (~0 for thigh bones
    // in this rig), so it has no visible effect here — applied for axis consistency.
    float thigh_pos = cl(Fr * (2.f - H) * Hips);
    out.pos[2][1] = thigh_pos;
    out.pos[7][1] = thigh_pos;

    // Calves [3,8]: posScale[0] = thigh bone length scale = leg_Y*0.95.
    // Kenshi RE: Calf has no explicit setBonePositionalSize because OGRE's setBoneSize on
    // Thigh propagates automatically to child joints. Our engine has no propagation → simulate.
    // ty formula in game.h: (s_leg_y-0.95)*0.9 = 0.855*(leg_Y-1) exactly matches this extension.
    float thigh_scale = leg_Y * 0.95f;
    float calf_XZ = cl((2.f - LgS) * LgB * Fr);
    if (calf_XZ < 0.30f) calf_XZ = 0.30f;  // prevent near-zero LBS collapse at min bulk
    // wy = thigh_scale only — leg bulk controls girth, not calf length (was thigh_scale*calf_XZ)
    // wx/wz = linear calf_XZ — quadratic (calf_XZ²) caused inflated-ball look at high bulk
    setBS(3, thigh_scale, calf_XZ, calf_XZ);
    setBS(8, thigh_scale, calf_XZ, calf_XZ);
    out.pos[3][0] = thigh_scale;
    out.pos[8][0] = thigh_scale;

    // Feet [4,9]: Kenshi RE — setBoneSize on Foot NOT found in leg slider function.
    // Only Feet slider (Ft) scales foot vertex geometry. H does NOT scale foot verts
    // (adding H caused foot to appear bloated at H=120 and confused bone-based grounding).
    // pos[4/9] propagates the calf extension so foot bone tracks the stretched calf tip.
    float FtS = cl(Ft);
    setBS(4, cl(LL * FtS), cl(FtS*FtS), cl(FtS*FtS));
    setBS(9, cl(LL * FtS), cl(FtS*FtS), cl(FtS*FtS));
    out.pos[4][0] = thigh_scale;  // foot tracks calf bone length extension
    out.pos[9][0] = thigh_scale;

    // Toes [5,10]
    float FtS2 = FtS * FtS;
    for (int ji = 5; ji <= 10; ji += 5) {
        out.bone[ji][0] = FtS2; out.bone[ji][1] = FtS2; out.bone[ji][2] = FtS2;
    }
    out.pos[5][2]  = FtS;
    out.pos[10][2] = FtS;

    // ── Torso ─────────────────────────────────────────────────────────────────
    setBS(12, H, comp(Hips,0.6f)*Fr, comp(Hips,0.6f)*St*Fr);
    setBS(13, H, Wa*Fr, St*Fr);
    out.pos[13][0] = H;  // Spine1 from Spine
    setBS(14, H, comp(Ch,0.45f)*Fr, comp(Ch,0.9f)*Fr);
    out.pos[14][0] = H;  // Spine2 from Spine1

    // Posture: Kenshi range 0-70, neutral=35. HIGH slider = hunch, LOW = erect.
    // RE: animation scrub timePos = animLength * pct * 0.01 (HIGH confidence).
    // Sign: (Pt-1) — positive lean_rad = forward hunch (HIGH body[4]); negative = erect.
    // S-curve: lumbar opposes thoracic (lordosis vs kyphosis). Head droops, not compensated.
    {
        float lean_raw = Pt - 1.f;  // >0 at HIGH body[4] = hunch; <0 at LOW = erect
        float lean_rad = lean_raw >= 0.f ? lean_raw * 0.40f : lean_raw * 0.25f;
        if (lean_rad >  0.40f) lean_rad =  0.40f;  // cap hunch: slider 70→99 beyond Kenshi
        if (lean_rad < -0.25f) lean_rad = -0.25f;  // cap erect: subtle military stance
        out.rot[12] = -lean_rad * 0.15f;  // Spine (lumbar): lordosis — opposes thoracic → S-curve
        out.rot[13] =  lean_rad * 0.55f;  // Spine1 (thoracic): main kyphosis arc
        out.rot[14] =  lean_rad * 0.40f;  // Spine2 (upper thoracic)
        out.rot[20] =  lean_rad * 0.35f;  // Neck: significant forward drop during hunch
        out.rot[21] =  lean_rad * 0.25f;  // Head: droops forward with spine (NOT compensated)
    }

    // ── Arms ──────────────────────────────────────────────────────────────────
    // Kenshi UI clamps arm bulk to ~30 minimum; below that LBS collapses arm verts
    // to bone center and idle-pose angle makes them appear horizontal (T-pose spread).
    float Ab_eff = Ab < 0.30f ? 0.30f : Ab;
    float AbFr = Ab_eff * Fr;
    float AbZ  = comp(Ab_eff, 1.5f) * Fr;

    // Clavicle length (wy) = shoulder width (Sh), NOT arm bulk.
    // Arm bulk drove clavicle outward, creating a sphere at the shoulder at high Ab.
    for (int ji = 15; ji <= 25; ji += 10) {
        out.bone[ji][0] = Sh; out.bone[ji][1] = 1.f; out.bone[ji][2] = 1.f;
    }
    float ChW = comp(Ch, 0.45f) * Fr;
    float ChD = comp(Ch, 0.9f)  * Fr;
    out.pos[15][0] = H * SS;  out.pos[15][1] = ChW;  out.pos[15][2] = ChD;
    out.pos[25][0] = H * SS;  out.pos[25][1] = ChW;  out.pos[25][2] = ChD;

    // RE only confirmed Z (depth) = comp(Ab,1.5) for UpperArm — no extra Fr multiplier.
    // Arm LENGTH (wy) tracks HEIGHT only. Arm girth (wx,wz) tracks Ab linearly.
    // pos[child][0] = parent wy = H, same pattern as calf pos = thigh_scale.
    for (int ji = 16; ji <= 26; ji += 10) {
        out.bone[ji][0] = H;    // wy: arm length = height only
        out.bone[ji][1] = AbFr; // wx: width — linear bulk×frame (no H, no square)
        out.bone[ji][2] = AbZ;  // wz: depth = comp(Ab,1.5)*Fr — RE confirmed, no extra *Ab
    }
    float arm_pos = cl(Sh * comp(Ch, 0.45f));
    out.pos[16][0] = arm_pos;
    out.pos[26][0] = arm_pos;

    for (int ji = 17; ji <= 27; ji += 10) {
        out.bone[ji][0] = H;    // wy: same height-driven length
        out.bone[ji][1] = AbFr; // wx: linear (no H, no square)
        out.bone[ji][2] = AbFr; // wz: linear (was AbFr² — too aggressive)
    }
    out.pos[17][0] = H;
    out.pos[27][0] = H;

    for (int ji = 18; ji <= 28; ji += 10) {
        out.bone[ji][0] = H*Hn*Hn;   // wy: hand length tracks height + hand size
        out.bone[ji][1] = AbFr*Hn;   // wx: removed extra H
        out.bone[ji][2] = AbFr*Hn*Hn;// wz
    }
    out.pos[18][0] = H;
    out.pos[28][0] = H;

    // ── Head/Neck ─────────────────────────────────────────────────────────────
    float Nc  = cl(face[2]  / 100.f);
    float Nw  = cl(face[3]  / 100.f);
    float Nl  = cl(face[4]  / 100.f);
    float jaw = cl(face[17] / 100.f);

    setBS(20, Nl, Nw*Fr, Nc*Fr);
    // NP controls neck HEIGHT (how far along spine the neck is) — not lateral offset.
    // pos[20][1] (lateral Y) was wrong: bind_t Y ≈ 0 so it had zero visible effect.
    out.pos[20][0] = H * comp(NP, 0.4f);

    float Hd  = cl(face[0] / 100.f);
    float FrH = comp(Fr, 0.25f);
    float Hsp = cl(face[1] / 100.f);
    setBS(21, FrH*Hd, FrH*Hd*Hsp, FrH*Hd);
    out.pos[21][0] = H;

    if (23 < MAX_SKIN_BONES) {
        out.bone[23][0] = FrH*Hd;
        out.bone[23][1] = FrH*Hd*Hsp*jaw;
        out.bone[23][2] = FrH*Hd;
    }
}

// ── CharCustomization_ApplyMorphs ────────────────────────────────────────────
// Port of kFaceMorphMap from editor_char_preview_sdlgpu.h.
// Each entry maps face[idx] → morph target name(s).
// Weight: if val > def → positive morph weight proportional to (val-def)/(hi-def).
//         if val < def and neg != nullptr → negative morph weight ∝ (def-val)/(def-lo).
//         otherwise → 0.

struct FaceMorphEntry { int idx; const char* pos; const char* neg; };

static const FaceMorphEntry kFaceMorphMap[] = {
    {  5, "big_eyes",           nullptr             },  // Eye size
    {  6, "narrow_eyes",        nullptr             },  // Eye shape
    {  7, "close_eyes",         nullptr             },  // Eye spacing
    {  8, "high_eyes",          nullptr             },  // Eye height
    {  9, "wide_nose",          nullptr             },  // Nose width
    { 10, "long_nose",          nullptr             },  // Nose length
    { 11, "arch_nose",          nullptr             },  // Nose depth
    { 12, "tiltup_nose",        "tiltdown_nose"     },  // Nose tip
    { 13, "wide_cheekbones",    "narrow_cheekbones" },  // Cheekbone
    { 14, "shallow_eyes",       nullptr             },  // Eyes depth
    { 15, "tiltup_brow",        "tiltdown_brow"     },  // Brow
    { 16, "high_brow",          "low_brow"          },  // Brow height
    { 18, "wide_mouth",         nullptr             },  // Mouth width
    { 20, "big_mouth",          nullptr             },  // Lips
    { 21, "overbite",           "underbite"         },  // Chin
    { 22, "tiltup_eyes",        "tiltdown_eyes"     },  // Eyes tilt
    { 23, "high_nose",          nullptr             },  // Nose pos.
};
static constexpr int kFaceMorphMapN = 17;

void CharCustomization_ApplyMorphs(const float face[CHARCC_FACE_N],
                                    const float face_def[CHARCC_FACE_N],
                                    const float face_lo[CHARCC_FACE_N],
                                    const float face_hi[CHARCC_FACE_N],
                                    SkinMesh&   mesh)
{
    for (int m = 0; m < kFaceMorphMapN; ++m) {
        const FaceMorphEntry& e = kFaceMorphMap[m];
        int fi = e.idx;
        if (fi < 0 || fi >= CHARCC_FACE_N) continue;

        float val = face[fi];
        float def = face_def[fi];
        float lo  = face_lo[fi];
        float hi  = face_hi[fi];

        if (val >= def) {
            // Positive side morph
            float range = hi - def;
            float w = (range > 0.001f) ? (val - def) / range : 0.f;
            if (w < 0.f) w = 0.f;
            if (w > 1.f) w = 1.f;
            if (e.pos) {
                // Find morph by name and set weight
                for (int i = 0; i < mesh.MorphCount(); ++i) {
                    if (strcmp(mesh.MorphName(i), e.pos) == 0) {
                        mesh.morph_weights[i] = w;
                        break;
                    }
                }
            }
            // Zero out the negative-side morph if it exists
            if (e.neg) {
                for (int i = 0; i < mesh.MorphCount(); ++i) {
                    if (strcmp(mesh.MorphName(i), e.neg) == 0) {
                        mesh.morph_weights[i] = 0.f;
                        break;
                    }
                }
            }
        } else {
            // Below neutral: zero positive side, apply negative if available
            if (e.pos) {
                for (int i = 0; i < mesh.MorphCount(); ++i) {
                    if (strcmp(mesh.MorphName(i), e.pos) == 0) {
                        mesh.morph_weights[i] = 0.f;
                        break;
                    }
                }
            }
            if (e.neg) {
                float range = def - lo;
                float w = (range > 0.001f) ? (def - val) / range : 0.f;
                if (w < 0.f) w = 0.f;
                if (w > 1.f) w = 1.f;
                for (int i = 0; i < mesh.MorphCount(); ++i) {
                    if (strcmp(mesh.MorphName(i), e.neg) == 0) {
                        mesh.morph_weights[i] = w;
                        break;
                    }
                }
            }
        }
    }
}

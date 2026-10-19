// SkinMesh — GLB loader with skinning data + CPU animation playback.
#include <monkey_dust/render/skin_mesh.h>
#include <monkey_dust/render/char_customization.h>
#include "cgltf.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>

// per NPC per frame). Thread-local: T2 async jobs run on worker threads concurrently
// with main-thread T0/T1 eval — a global would race. thread_local gives each thread
// its own copy with zero overhead after the first access.
static thread_local float g_bone_world_scratch[MAX_SKIN_BONES][16];

// ── Math helpers ─────────────────────────────────────────────────────────────

void SkinMesh::mat4_identity(float* m) {
    memset(m, 0, 64);
    m[0] = m[5] = m[10] = m[15] = 1.f;
}

void SkinMesh::mat4_from_trs(float* m, const float* t, const float* q, const float* s) {
    float qx=q[0],qy=q[1],qz=q[2],qw=q[3];
    float x2=qx*2,y2=qy*2,z2=qz*2;
    float xx=qx*x2,yy=qy*y2,zz=qz*z2;
    float xy=qx*y2,xz=qx*z2,yz=qy*z2;
    float wx=qw*x2,wy=qw*y2,wz=qw*z2;
    // Column-major: each column scaled by the corresponding scale component
    m[0]=(1-(yy+zz))*s[0]; m[1]=(xy+wz)*s[0];    m[2]=(xz-wy)*s[0];    m[3]=0;
    m[4]=(xy-wz)*s[1];     m[5]=(1-(xx+zz))*s[1]; m[6]=(yz+wx)*s[1];    m[7]=0;
    m[8]=(xz+wy)*s[2];     m[9]=(yz-wx)*s[2];     m[10]=(1-(xx+yy))*s[2];m[11]=0;
    m[12]=t[0];            m[13]=t[1];             m[14]=t[2];            m[15]=1;
}

void SkinMesh::mat4_mul(float* out, const float* a, const float* b) {
    float tmp[16];
    for (int row = 0; row < 4; ++row)
        for (int col = 0; col < 4; ++col) {
            float s = 0;
            for (int k = 0; k < 4; ++k) s += a[k*4+row] * b[col*4+k];
            tmp[col*4+row] = s;
        }
    memcpy(out, tmp, 64);
}

void SkinMesh::quat_lerp(float* out, const float* a, const float* b, float t) {
    float dot = a[0]*b[0]+a[1]*b[1]+a[2]*b[2]+a[3]*b[3];
    float s   = (dot < 0.f) ? -1.f : 1.f;
    for (int i = 0; i < 4; ++i) out[i] = a[i]*(1.f-t) + b[i]*s*t;
    float len = sqrtf(out[0]*out[0]+out[1]*out[1]+out[2]*out[2]+out[3]*out[3]);
    if (len > 1e-9f) for (int i = 0; i < 4; ++i) out[i] /= len;
}

void SkinMesh::lerp3(float* out, const float* a, const float* b, float t) {
    for (int i = 0; i < 3; ++i) out[i] = a[i]*(1.f-t) + b[i]*t;
}

float SkinMesh::slerp_t(const SkinTrack& tr, float time_s,
                         float* out_t, float* out_q) {
    if (tr.count == 0) { out_t[0]=out_t[1]=out_t[2]=0; out_q[0]=out_q[1]=out_q[2]=0; out_q[3]=1; return 0; }
    if (tr.count == 1) {
        const auto& k = tr.kf[0];
        out_t[0]=k.tx; out_t[1]=k.ty; out_t[2]=k.tz;
        out_q[0]=k.qx; out_q[1]=k.qy; out_q[2]=k.qz; out_q[3]=k.qw;
        return 0;
    }
    // Find bracket
    int lo = 0;
    for (int i = 0; i < tr.count-1; ++i) { if (tr.kf[i+1].t > time_s) { lo = i; break; } lo = i; }
    int hi = lo + 1;
    if (hi >= tr.count) hi = tr.count - 1;
    float span = tr.kf[hi].t - tr.kf[lo].t;
    float f    = (span > 1e-6f) ? (time_s - tr.kf[lo].t) / span : 0.f;
    if (f < 0) f = 0; if (f > 1) f = 1;
    const float ta[3] = { tr.kf[lo].tx, tr.kf[lo].ty, tr.kf[lo].tz };
    const float tb[3] = { tr.kf[hi].tx, tr.kf[hi].ty, tr.kf[hi].tz };
    const float qa[4] = { tr.kf[lo].qx, tr.kf[lo].qy, tr.kf[lo].qz, tr.kf[lo].qw };
    const float qb[4] = { tr.kf[hi].qx, tr.kf[hi].qy, tr.kf[hi].qz, tr.kf[hi].qw };
    lerp3(out_t, ta, tb, f);
    quat_lerp(out_q, qa, qb, f);
    return f;
}

// ── Morph target helpers ─────────────────────────────────────────────────────

// Extract targetNames array from glTF mesh extras JSON:
//   {"targetNames":["wide_cheekbones","narrow_cheekbones",...]}
static void s_parse_morph_names(const char* extras_json,
                                 char names[][48], int max_n, int* out_count) {
    *out_count = 0;
    if (!extras_json) return;
    const char* p = strstr(extras_json, "\"targetNames\"");
    if (!p) return;
    p = strchr(p, '['); if (!p) return; ++p;
    int idx = 0;
    while (*p && idx < max_n) {
        while (*p && *p != '"' && *p != ']') ++p;
        if (!*p || *p == ']') break;
        ++p; // skip opening quote
        int len = 0;
        while (*p && *p != '"' && len < 47) names[idx][len++] = *p++;
        names[idx][len] = '\0';
        if (*p == '"') ++p; // skip closing quote
        ++idx;
    }
    *out_count = idx;
}

// ── GLB loading ──────────────────────────────────────────────────────────────

bool SkinMesh::LoadGLB(const char* path) {
    loaded = false;
    if (!path) return false;

    cgltf_options opts = {};
    cgltf_data*   data = nullptr;
    if (cgltf_parse_file(&opts, path, &data) != cgltf_result_success) {
        fprintf(stderr, "[SkinMesh] parse failed: %s\n", path);
        return false;
    }
    if (cgltf_load_buffers(&opts, data, path) != cgltf_result_success) {
        fprintf(stderr, "[SkinMesh] buffer load failed: %s\n", path);
        cgltf_free(data); return false;
    }

    // ── Mesh primitive ────────────────────────────────────────────────────
    cgltf_primitive* prim = nullptr;
    for (cgltf_size mi = 0; mi < data->meshes_count && !prim; ++mi)
        for (cgltf_size pi = 0; pi < data->meshes[mi].primitives_count && !prim; ++pi) {
            cgltf_primitive* p = &data->meshes[mi].primitives[pi];
            if (p->type != cgltf_primitive_type_triangles || !p->indices) continue;
            bool hp=false,hn=false,hj=false,hw=false;
            for (cgltf_size ai = 0; ai < p->attributes_count; ++ai) {
                auto t = p->attributes[ai].type;
                if (t==cgltf_attribute_type_position) hp=true;
                if (t==cgltf_attribute_type_normal)   hn=true;
                if (t==cgltf_attribute_type_joints)   hj=true;
                if (t==cgltf_attribute_type_weights)  hw=true;
            }
            if (hp && hn) prim = p;  // joints/weights optional but expected
        }
    if (!prim) { fprintf(stderr,"[SkinMesh] no valid prim: %s\n",path); cgltf_free(data); return false; }

    // Validate expected attributes — missing normals → vN = (0,0,0) → normalize = NaN → white.
    {
        bool hp=false,hn=false,hj=false,hw=false;
        for (cgltf_size ai=0;ai<prim->attributes_count;++ai) {
            auto t=prim->attributes[ai].type;
            if(t==cgltf_attribute_type_position) hp=true;
            if(t==cgltf_attribute_type_normal)   hn=true;
            if(t==cgltf_attribute_type_joints)   hj=true;
            if(t==cgltf_attribute_type_weights)  hw=true;
        }
        if(!hn) fprintf(stderr,"[SkinMesh] WARN %s: no NORMAL — shader will produce NaN normals\n",path);
        if(!hj) fprintf(stderr,"[SkinMesh] WARN %s: no JOINTS — bone skinning disabled\n",path);
        if(!hw) fprintf(stderr,"[SkinMesh] WARN %s: no WEIGHTS — bone skinning disabled\n",path);
    }

    cgltf_accessor *pos_acc=nullptr,*nor_acc=nullptr,*uv_acc=nullptr,*jnt_acc=nullptr,*wgt_acc=nullptr;
    for (cgltf_size ai = 0; ai < prim->attributes_count; ++ai) {
        auto& a = prim->attributes[ai];
        if (a.type==cgltf_attribute_type_position)  pos_acc=a.data;
        if (a.type==cgltf_attribute_type_normal)    nor_acc=a.data;
        if (a.type==cgltf_attribute_type_texcoord)  uv_acc =a.data;
        if (a.type==cgltf_attribute_type_joints)    jnt_acc=a.data;
        if (a.type==cgltf_attribute_type_weights)   wgt_acc=a.data;
    }

    cgltf_size nv = pos_acc->count;
    if (nv == 0 || nv > 131072) { fprintf(stderr,"[SkinMesh] bad vert count %zu\n",nv); cgltf_free(data); return false; }

    static SkinVertex s_verts[131072];
    for (cgltf_size i = 0; i < nv; ++i) {
        float p[3]={0,0,0}, n[3]={0,1,0}, uv[2]={0,0}, w[4]={1,0,0,0};
        unsigned int ji[4]={0,0,0,0};
        cgltf_accessor_read_float(pos_acc, i, p, 3);
        cgltf_accessor_read_float(nor_acc, i, n, 3);
        if (uv_acc)  cgltf_accessor_read_float(uv_acc,  i, uv, 2);
        if (wgt_acc) cgltf_accessor_read_float(wgt_acc, i, w, 4);
        if (jnt_acc) cgltf_accessor_read_uint (jnt_acc, i, ji, 4);
        s_verts[i].x=p[0]; s_verts[i].y=p[1]; s_verts[i].z=p[2];
        s_verts[i].nx=n[0]; s_verts[i].ny=n[1]; s_verts[i].nz=n[2];
        s_verts[i].u=uv[0]; s_verts[i].v=uv[1];
        s_verts[i].j[0]=(uint8_t)ji[0]; s_verts[i].j[1]=(uint8_t)ji[1];
        s_verts[i].j[2]=(uint8_t)ji[2]; s_verts[i].j[3]=(uint8_t)ji[3];
        s_verts[i].w[0]=w[0]; s_verts[i].w[1]=w[1];
        s_verts[i].w[2]=w[2]; s_verts[i].w[3]=w[3];
    }
    vbo.Init(0x8892u, s_verts, (uint32_t)(nv * sizeof(SkinVertex)));
    cpu_verts_  = s_verts;
    vert_count_ = (uint32_t)nv;

    // ── Morph targets ─────────────────────────────────────────────────────────
    free(morph_deltas_); morph_deltas_ = nullptr;
    morph_count_ = 0;
    memset(morph_weights, 0, sizeof(morph_weights));
    memset(morph_names_,  0, sizeof(morph_names_));

    int n_targets = (int)prim->targets_count;
    if (n_targets > MAX_MORPH_TARGETS) n_targets = MAX_MORPH_TARGETS;
    if (n_targets > 0) {
        // Parse targetNames from mesh extras JSON
        const char* extras_json = nullptr;
        for (cgltf_size mi2 = 0; mi2 < data->meshes_count; ++mi2)
            for (cgltf_size pi2 = 0; pi2 < data->meshes[mi2].primitives_count; ++pi2)
                if (&data->meshes[mi2].primitives[pi2] == prim) {
                    extras_json = data->meshes[mi2].extras.data; goto found_mesh;
                }
        found_mesh:
        int names_found = 0;
        s_parse_morph_names(extras_json, morph_names_, n_targets, &names_found);

        size_t delta_floats = (size_t)n_targets * nv * 3;
        size_t delta_bytes  = delta_floats * sizeof(float);
        morph_deltas_ = (float*)aligned_alloc(16, (delta_bytes + 15) & ~(size_t)15);
        if (morph_deltas_) memset(morph_deltas_, 0, delta_bytes);
        if (morph_deltas_) {
            for (int mt = 0; mt < n_targets; ++mt) {
                cgltf_morph_target& tgt = prim->targets[mt];
                for (cgltf_size ai = 0; ai < tgt.attributes_count; ++ai) {
                    if (tgt.attributes[ai].type != cgltf_attribute_type_position) continue;
                    cgltf_accessor* dacc = tgt.attributes[ai].data;
                    float* base = morph_deltas_ + (size_t)mt * nv * 3;
                    cgltf_size read_n = dacc->count < nv ? dacc->count : nv;
                    for (cgltf_size vi = 0; vi < read_n; ++vi)
                        cgltf_accessor_read_float(dacc, vi, base + vi * 3, 3);
                    break;
                }
                // Fill name from fallback index if not found in extras
                if (mt >= names_found || morph_names_[mt][0] == '\0')
                    snprintf(morph_names_[mt], 48, "morph_%d", mt);
            }
            morph_count_ = n_targets;
        }
        fprintf(stdout,"[SkinMesh] %d morph targets loaded\n", morph_count_);
    }

    cgltf_size ni = prim->indices->count;
    // Unlike nv (bounds-checked above against the 131072-vertex cap), ni was
    // used unchecked to write into the 1<<20-element static s_idx16/s_idx32
    // buffers -- a dense mesh with more indices than vertices (independent
    // caps: index count isn't bounded by vertex count) could overflow them.
    if (ni == 0 || ni > (1u<<20)) {
        fprintf(stderr,"[SkinMesh] bad index count %zu\n", ni);
        cgltf_free(data);
        return false;
    }
    indices_u16 = (nv <= 65535);
    index_count = (uint32_t)ni;
    if (indices_u16) {
        static uint16_t s_idx16[1<<20];
        for (cgltf_size i=0;i<ni;++i) s_idx16[i]=(uint16_t)cgltf_accessor_read_index(prim->indices,i);
        ibo.Init(0x8893u, s_idx16, (uint32_t)(ni*2));
    } else {
        static uint32_t s_idx32[1<<20];
        for (cgltf_size i=0;i<ni;++i) s_idx32[i]=(uint32_t)cgltf_accessor_read_index(prim->indices,i);
        ibo.Init(0x8893u, s_idx32, (uint32_t)(ni*4));
    }

    // ── Skin / inverse bind matrices ──────────────────────────────────────
    for (int i=0;i<MAX_SKIN_BONES;++i) { mat4_identity(inv_bind_[i]); parent_[i]=-1; }
    if (data->skins_count > 0) {
        cgltf_skin& sk = data->skins[0];
        bone_count = (int)sk.joints_count;
        if (bone_count > MAX_SKIN_BONES) bone_count = MAX_SKIN_BONES;

        // Inverse bind matrices from accessor
        if (sk.inverse_bind_matrices) {
            for (int i=0;i<bone_count;++i)
                cgltf_accessor_read_float(sk.inverse_bind_matrices,(cgltf_size)i,inv_bind_[i],16);
        }

        // Parent array, bind-pose local TRS, bone names
        for (int i=0;i<bone_count;++i) {
            cgltf_node* node = sk.joints[i];
            // Bone name for upper/lower body masking
            if (node->name) strncpy(bone_names[i], node->name, 31);
            else bone_names[i][0] = '\0';
            bone_hash_[i] = BoneHash(bone_names[i]);  // KEN-MORPH-1
            // Find parent index within joints array
            parent_[i] = -1;
            if (node->parent) {
                for (int j=0;j<bone_count;++j)
                    if (sk.joints[j] == node->parent) { parent_[i]=j; break; }
            }
            // Local TRS from node
            if (node->has_translation) { bind_t_[i][0]=node->translation[0]; bind_t_[i][1]=node->translation[1]; bind_t_[i][2]=node->translation[2]; }
            else { bind_t_[i][0]=bind_t_[i][1]=bind_t_[i][2]=0; }
            if (node->has_rotation) { bind_q_[i][0]=node->rotation[0]; bind_q_[i][1]=node->rotation[1]; bind_q_[i][2]=node->rotation[2]; bind_q_[i][3]=node->rotation[3]; }
            else { bind_q_[i][0]=bind_q_[i][1]=bind_q_[i][2]=0; bind_q_[i][3]=1; }
            if (node->has_scale) { bind_s_[i][0]=node->scale[0]; bind_s_[i][1]=node->scale[1]; bind_s_[i][2]=node->scale[2]; }
            else { bind_s_[i][0]=bind_s_[i][1]=bind_s_[i][2]=1.f; }
        }

        // Build topological processing order (parent always before child).
        // Needed because GLB joint arrays are not required to be parent-first.
        bool topo_added[MAX_SKIN_BONES] = {};
        int  n_order = 0;
        // Add roots first
        for (int i = 0; i < bone_count; ++i)
            if (parent_[i] < 0) { process_order_[n_order++] = i; topo_added[i] = true; }
        // Then repeatedly add bones whose parent is already in the order
        for (int pass = 0; pass < bone_count && n_order < bone_count; ++pass)
            for (int i = 0; i < bone_count; ++i)
                if (!topo_added[i] && parent_[i] >= 0 && topo_added[parent_[i]])
                    { process_order_[n_order++] = i; topo_added[i] = true; }
    }

    // ── Animations ────────────────────────────────────────────────────────
    clip_count_ = 0;
    for (cgltf_size ai=0; ai<data->animations_count && clip_count_<MAX_SKIN_CLIPS; ++ai) {
        cgltf_animation& ca = data->animations[ai];
        SkinClip& cl = clips_[clip_count_];
        memset(&cl,0,sizeof(cl));
        if (ca.name) strncpy(cl.name, ca.name, sizeof(cl.name)-1);
        cl.bone_count = bone_count;

        for (cgltf_size chi=0; chi<ca.channels_count; ++chi) {
            cgltf_animation_channel& ch = ca.channels[chi];
            if (!ch.target_node || !ch.sampler) continue;
            if (ch.target_path != cgltf_animation_path_type_translation &&
                ch.target_path != cgltf_animation_path_type_rotation) continue;

            // Map node → bone index
            int bi = -1;
            if (data->skins_count > 0) {
                for (int j=0;j<bone_count;++j)
                    if (data->skins[0].joints[j] == ch.target_node) { bi=j; break; }
            }
            if (bi<0 || bi>=MAX_SKIN_BONES) continue;

            cgltf_accessor* times_acc = ch.sampler->input;
            cgltf_accessor* vals_acc  = ch.sampler->output;
            int nkf = (int)times_acc->count;
            if (nkf > MAX_SKIN_KF) nkf = MAX_SKIN_KF;

            SkinTrack& tr = cl.tracks[bi];

            for (int k=0;k<nkf;++k) {
                float t_k = 0;
                cgltf_accessor_read_float(times_acc,(cgltf_size)k,&t_k,1);
                if (t_k > cl.duration) cl.duration = t_k;

                if (ch.target_path == cgltf_animation_path_type_translation) {
                    float tv[3]={0,0,0};
                    cgltf_accessor_read_float(vals_acc,(cgltf_size)k,tv,3);
                    // Fill or extend keyframe slot
                    if (tr.count <= k) {
                        tr.kf[k] = {t_k, tv[0],tv[1],tv[2], 0,0,0,1};
                        tr.count = k+1;
                    } else {
                        tr.kf[k].t=t_k; tr.kf[k].tx=tv[0]; tr.kf[k].ty=tv[1]; tr.kf[k].tz=tv[2];
                    }
                } else { // rotation
                    float qv[4]={0,0,0,1};
                    cgltf_accessor_read_float(vals_acc,(cgltf_size)k,qv,4);
                    if (tr.count <= k) {
                        // Use bind_t so positional scales (setBonePositionalSize) work on
                        // rotation-only bones. A later translation channel overwrites tx/ty/tz.
                        tr.kf[k] = {t_k, bind_t_[bi][0],bind_t_[bi][1],bind_t_[bi][2], qv[0],qv[1],qv[2],qv[3]};
                        tr.count = k+1;
                    } else {
                        tr.kf[k].t=t_k; tr.kf[k].qx=qv[0]; tr.kf[k].qy=qv[1]; tr.kf[k].qz=qv[2]; tr.kf[k].qw=qv[3];
                    }
                }
            }
        }
        ++clip_count_;
    }

    cgltf_free(data);
    loaded = true;
    fprintf(stdout,"[SkinMesh] %s  verts=%u idx=%u bones=%d clips=%d\n",
            path,(unsigned)nv,(unsigned)ni,bone_count,clip_count_);
    for (int i=0;i<bone_count;++i)
        fprintf(stdout,"  bone[%2d] parent=%2d  '%s'\n", i, parent_[i], bone_names[i]);
    return true;
}

void SkinMesh::Shutdown() {
    vbo.Shutdown(); ibo.Shutdown();
    free(morph_deltas_); morph_deltas_ = nullptr;
    index_count=0; loaded=false; bone_count=0; clip_count_=0; morph_count_=0;
}

void SkinMesh::WriteMorphedVerts(SkinVertex* out) const {
    memcpy(out, cpu_verts_, vert_count_ * sizeof(SkinVertex));
    for (int m = 0; m < morph_count_; ++m) {
        float w = morph_weights[m];
        if (w == 0.f) continue;
        const float* deltas = morph_deltas_ + (size_t)m * vert_count_ * 3;
        for (uint32_t v = 0; v < vert_count_; ++v) {
            out[v].x  += w * deltas[v*3+0];
            out[v].y  += w * deltas[v*3+1];
            out[v].z  += w * deltas[v*3+2];
        }
    }
}

int SkinMesh::ClipIndexByName(const char* name) const {
    for (int i=0;i<clip_count_;++i)
        if (strcmp(clips_[i].name,name)==0) return i;
    return -1;
}

// KEN-MORPH-1: MurmurHash3_x86_32 with Kenshi bone-name seed 0x3a8efa67.
uint32_t SkinMesh::BoneHash(const char* key) {
    const uint8_t* data = (const uint8_t*)key;
    int len = 0; while (key[len]) ++len;
    const int nblocks = len / 4;
    uint32_t h = 0x3a8efa67u;
    const uint32_t c1 = 0xcc9e2d51u, c2 = 0x1b873593u;
    for (int i = 0; i < nblocks; ++i) {
        uint32_t k; memcpy(&k, data + i*4, 4);
        k *= c1; k = (k<<15)|(k>>17); k *= c2;
        h ^= k; h = (h<<13)|(h>>19); h = h*5 + 0xe6546b64u;
    }
    const uint8_t* tail = data + nblocks*4;
    uint32_t k = 0;
    switch (len & 3) {
        case 3: k ^= (uint32_t)tail[2] << 16; // fallthrough
        case 2: k ^= (uint32_t)tail[1] << 8;  // fallthrough
        case 1: k ^= tail[0]; k *= c1; k = (k<<15)|(k>>17); k *= c2; h ^= k; break;
        default: break;
    }
    h ^= (uint32_t)len;
    h ^= h>>16; h *= 0x85ebca6bu; h ^= h>>13; h *= 0xc2b2ae35u; h ^= h>>16;
    return h;
}

int SkinMesh::FindBoneByName(const char* name) const {
    const uint32_t h = BoneHash(name);
    for (int i = 0; i < bone_count; ++i)
        if (bone_hash_[i] == h && strcmp(bone_names[i], name) == 0) return i;
    return -1;
}

// ── GetFinalBones ────────────────────────────────────────────────────────────

void SkinMesh::GetFinalBones(int clip_idx, float time_s, float* out) const {
    // out: float[MAX_SKIN_BONES * 16], column-major mat4 per bone

    // Clamp / loop time. Clips with dur < 0.05s are single-frame reference
    // poses (2 near-identical keyframes from OGRE export) — always sample t=0
    // to avoid 30 Hz oscillation between the two keyframes.
    float t = time_s;
    if (clip_idx >= 0 && clip_idx < clip_count_) {
        float dur = clips_[clip_idx].duration;
        if (dur > 0.05f) t = fmodf(t, dur); else t = 0.f;
    }

    float (&world)[MAX_SKIN_BONES][16] = g_bone_world_scratch;
    float local[16];

    // Process in topological order (parent always before child)
    for (int oi = 0; oi < bone_count; ++oi) {
        int i = process_order_[oi];
        float lt[3], lq[4];

        if (clip_idx >= 0 && clip_idx < clip_count_) {
            const SkinClip& cl = clips_[clip_idx];
            const SkinTrack& tr = cl.tracks[i];
            if (tr.count > 0) {
                slerp_t(tr, t, lt, lq);
            } else {
                lt[0]=bind_t_[i][0]; lt[1]=bind_t_[i][1]; lt[2]=bind_t_[i][2];
                lq[0]=bind_q_[i][0]; lq[1]=bind_q_[i][1]; lq[2]=bind_q_[i][2]; lq[3]=bind_q_[i][3];
            }
        } else {
            lt[0]=bind_t_[i][0]; lt[1]=bind_t_[i][1]; lt[2]=bind_t_[i][2];
            lq[0]=bind_q_[i][0]; lq[1]=bind_q_[i][1]; lq[2]=bind_q_[i][2]; lq[3]=bind_q_[i][3];
        }

        mat4_from_trs(local, lt, lq, bind_s_[i]);

        int pi = parent_[i];
        if (pi < 0) memcpy(world[i], local, 64);
        else        mat4_mul(world[i], world[pi], local);

        mat4_mul(out + i*16, world[i], inv_bind_[i]);
    }
    // Identity for unused bone slots
    for (int i = bone_count; i < MAX_SKIN_BONES; ++i)
        mat4_identity(out + i*16);

}


void SkinMesh::GetFinalBonesFull(int clip_idx, float time_s,
                                  float* out_bones,
                                  float* out_model_xyz,
                                  float* out_world_mats) const {
    float t = time_s;
    if (clip_idx >= 0 && clip_idx < clip_count_) {
        float dur = clips_[clip_idx].duration;
        if (dur > 0.05f) t = fmodf(t, dur); else t = 0.f;
    }

    float (&world)[MAX_SKIN_BONES][16] = g_bone_world_scratch;
    float local[16];

    for (int oi = 0; oi < bone_count; ++oi) {
        int i = process_order_[oi];
        float lt[3], lq[4];

        if (clip_idx >= 0 && clip_idx < clip_count_) {
            const SkinClip& cl = clips_[clip_idx];
            const SkinTrack& tr = cl.tracks[i];
            if (tr.count > 0) {
                slerp_t(tr, t, lt, lq);
            } else {
                lt[0]=bind_t_[i][0]; lt[1]=bind_t_[i][1]; lt[2]=bind_t_[i][2];
                lq[0]=bind_q_[i][0]; lq[1]=bind_q_[i][1]; lq[2]=bind_q_[i][2]; lq[3]=bind_q_[i][3];
            }
        } else {
            lt[0]=bind_t_[i][0]; lt[1]=bind_t_[i][1]; lt[2]=bind_t_[i][2];
            lq[0]=bind_q_[i][0]; lq[1]=bind_q_[i][1]; lq[2]=bind_q_[i][2]; lq[3]=bind_q_[i][3];
        }

        mat4_from_trs(local, lt, lq, bind_s_[i]);
        int pi = parent_[i];
        if (pi < 0) memcpy(world[i], local, 64);
        else        mat4_mul(world[i], world[pi], local);

        mat4_mul(out_bones + i*16, world[i], inv_bind_[i]);

        // Model-space position = translation column of world[i]
        if (out_model_xyz) {
            out_model_xyz[i*3+0] = world[i][12];
            out_model_xyz[i*3+1] = world[i][13];
            out_model_xyz[i*3+2] = world[i][14];
        }
        if (out_world_mats) {
            memcpy(out_world_mats + i*16, world[i], 64);
        }
    }
    for (int i = bone_count; i < MAX_SKIN_BONES; ++i) {
        mat4_identity(out_bones + i*16);
        if (out_model_xyz)  { out_model_xyz[i*3]=out_model_xyz[i*3+1]=out_model_xyz[i*3+2]=0.f; }
        if (out_world_mats) { mat4_identity(out_world_mats + i*16); }
    }
}

void SkinMesh::PatchBoneIK(int bone_idx, const float* new_world_16,
                             float* out_bones) const {
    if (bone_idx < 0 || bone_idx >= bone_count) return;
    mat4_mul(out_bones + bone_idx * 16, new_world_16, inv_bind_[bone_idx]);
}

float SkinMesh::LastBoneWorldY(int bone_idx) const {
    if (bone_idx < 0 || bone_idx >= MAX_SKIN_BONES) return 0.f;
    return g_bone_world_scratch[bone_idx][13];
}





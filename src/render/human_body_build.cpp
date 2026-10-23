#include <monkey_dust/render/human_body_build.h>
#include <monkey_dust/io/kenshi_resources.h>
#include <monkey_dust/io/ogre_mesh.h>
#include <monkey_dust/io/ogre_skeleton.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <set>

namespace {
constexpr double kScale = 0.1;   // Kenshi units -> metres
const char* const kClips[] = { "breathing", "breathing noarms", "combat stance ref", "combatstance", "idle_stand_normal", "jog lower", "jog upper", "neck set", "postures", "run lower", "run upper", "shoulder set", "walk lower", "walk upper" };

void QuatMul(const double a[4], const double b[4], double o[4]) {   // x y z w, as tools/md_convert.py quat_mul
    o[0] = a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1];
    o[1] = a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0];
    o[2] = a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3];
    o[3] = a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2];
}
void QuatNorm(double q[4]) { const double l = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]); if (l < 1e-9) { q[0] = q[1] = q[2] = 0; q[3] = 1; return; } for (int i = 0; i < 4; ++i) q[i] /= l; }
void FromTrs(const double t[3], const double q[4], double m[16]) {   // tools/md_convert.py mat4_from_trs with scale 1
    const double x2 = q[0] * 2, y2 = q[1] * 2, z2 = q[2] * 2, xx = q[0] * x2, yy = q[1] * y2, zz = q[2] * z2, xy = q[0] * y2, xz = q[0] * z2, yz = q[1] * z2, wx = q[3] * x2, wy = q[3] * y2, wz = q[3] * z2;
    m[0] = 1 - (yy + zz); m[1] = xy + wz; m[2] = xz - wy; m[3] = 0; m[4] = xy - wz; m[5] = 1 - (xx + zz); m[6] = yz + wx; m[7] = 0;
    m[8] = xz + wy; m[9] = yz - wx; m[10] = 1 - (xx + yy); m[11] = 0; m[12] = t[0]; m[13] = t[1]; m[14] = t[2]; m[15] = 1;
}
void Mul(const double a[16], const double b[16], double o[16]) { for (int c = 0; c < 4; ++c) for (int r = 0; r < 4; ++r) { double s = 0; for (int k = 0; k < 4; ++k) s += a[k * 4 + r] * b[c * 4 + k]; o[c * 4 + r] = s; } }
void Inverse(const double m[16], double o[16]) {   // cofactor expansion, as md_convert.py mat4_inv
    auto det3 = [](const double a[3][3]) { return a[0][0] * (a[1][1] * a[2][2] - a[1][2] * a[2][1]) - a[0][1] * (a[1][0] * a[2][2] - a[1][2] * a[2][0]) + a[0][2] * (a[1][0] * a[2][1] - a[1][1] * a[2][0]); };
    double cof[16];
    for (int r = 0; r < 4; ++r) for (int c = 0; c < 4; ++c) {
        int rows[3], cols[3], ri = 0, ci = 0; for (int i = 0; i < 4; ++i) { if (i != r) rows[ri++] = i; if (i != c) cols[ci++] = i; }
        double s[3][3]; for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j) s[i][j] = m[rows[i] * 4 + cols[j]];
        cof[r * 4 + c] = (((r + c) & 1) ? -1.0 : 1.0) * det3(s);
    }
    double det = 0; for (int c = 0; c < 4; ++c) det += m[c] * cof[c];
    if (std::fabs(det) < 1e-12) { for (int i = 0; i < 16; ++i) o[i] = m[i]; return; }
    for (int c = 0; c < 4; ++c) for (int r = 0; r < 4; ++r) o[c * 4 + r] = cof[r * 4 + c] / det;
}

}  // namespace

bool BuildHumanBody(const std::string& root, HumanBodyData& o) {
    o = HumanBodyData{};
    md::KenshiResources res; res.Index(root);
    const std::string mesh_path = res.Path("human_male.mesh");
    if (mesh_path.empty()) return false;
    md::ogre::Mesh m; md::ogre::Error err;
    if (!md::ogre::ReadMesh(mesh_path, m, err) || m.submeshes.empty()) return false;
    const md::ogre::SubMesh& sm = m.submeshes[0];
    const md::ogre::Geometry& g = sm.use_shared ? m.shared : sm.geometry;
    std::vector<float> p, n, uv;
    if (!g.Positions(p) || !g.Normals(n)) return false;
    o.nv = g.vertex_count;
    const bool has_uv = g.Uvs(0, uv);
    o.pos.resize((size_t)o.nv * 3); o.nrm.resize((size_t)o.nv * 3); o.uv.assign((size_t)o.nv * 2, 0.f);
    for (size_t i = 0; i < (size_t)o.nv * 3; ++i) { o.pos[i] = (float)((double)p[i] * kScale); o.nrm[i] = n[i]; }
    if (has_uv) for (size_t i = 0; i < (size_t)o.nv * 2; ++i) o.uv[i] = uv[i];
    o.idx = sm.indices;
    // top four bone weights per vertex, sorted by (weight, bone) descending, renormalised
    const std::vector<md::ogre::BoneAssignment>& ba = sm.use_shared ? m.bone_assignments : sm.bone_assignments;
    std::vector<std::vector<std::pair<double, int>>> per(o.nv);
    for (const md::ogre::BoneAssignment& a : ba) if (a.vertex < o.nv) per[a.vertex].push_back({ (double)a.weight, (int)a.bone });
    o.joint.assign((size_t)o.nv * 4, 0); o.weight.assign((size_t)o.nv * 4, 0.f);
    for (uint32_t v = 0; v < o.nv; ++v) {
        auto& ws = per[v];
        std::sort(ws.begin(), ws.end(), [](const auto& a, const auto& b) { return a > b; });
        if (ws.size() > 4) ws.resize(4);
        while (ws.size() < 4) ws.push_back({ 0.0, 0 });
        double tot = 0; for (auto& w : ws) tot += w.first; if (tot == 0) tot = 1.0;
        for (int k = 0; k < 4; ++k) { o.joint[v * 4 + k] = (uint8_t)ws[k].second; o.weight[v * 4 + k] = (float)(ws[k].first / tot); }
    }
    // skeleton
    std::string skel_name = m.has_skeleton ? m.skeleton : "male_skeleton.skeleton";
    const std::string skel_path = res.Path(skel_name);
    md::ogre::Skeleton sk;
    if (skel_path.empty() || !md::ogre::ReadSkeleton(skel_path, sk, err)) return false;
    const size_t nb = sk.bones.size();
    std::vector<int> by_handle(65536, -1);
    for (size_t i = 0; i < nb; ++i) { o.bone_names.push_back(sk.bones[i].name); by_handle[sk.bones[i].handle] = (int)i; }
    o.parent.assign(nb, -1); o.bind_t.resize(nb * 3); o.bind_q.resize(nb * 4);
    std::vector<std::array<double, 7>> bind(nb);
    for (size_t i = 0; i < nb; ++i) {
        const md::ogre::Bone& b = sk.bones[i];
        if (b.has_parent) o.parent[i] = by_handle[b.parent];
        double q[4] = { b.orientation[0], b.orientation[1], b.orientation[2], b.orientation[3] };
        QuatNorm(q);
        for (int k = 0; k < 3; ++k) { bind[i][k] = (double)b.position[k] * kScale; o.bind_t[i * 3 + k] = (float)bind[i][k]; }
        for (int k = 0; k < 4; ++k) { bind[i][3 + k] = q[k]; o.bind_q[i * 4 + k] = (float)q[k]; }
    }
    std::vector<std::array<double, 16>> world(nb); std::vector<bool> done(nb, false);
    std::function<void(size_t)> build = [&](size_t i) {
        if (done[i]) return;
        double t[3] = { bind[i][0], bind[i][1], bind[i][2] }, q[4] = { bind[i][3], bind[i][4], bind[i][5], bind[i][6] }, local[16];
        FromTrs(t, q, local);
        if (o.parent[i] < 0) { for (int k = 0; k < 16; ++k) world[i][k] = local[k]; }
        else { build((size_t)o.parent[i]); Mul(world[o.parent[i]].data(), local, world[i].data()); }
        done[i] = true;
    };
    o.inv_bind.resize(nb * 16);
    for (size_t i = 0; i < nb; ++i) { build(i); double inv[16]; Inverse(world[i].data(), inv); for (int k = 0; k < 16; ++k) o.inv_bind[i * 16 + k] = (float)inv[k]; }
    // clips
    const std::set<std::string> wanted(std::begin(kClips), std::end(kClips));
    for (const md::ogre::Animation& a : sk.animations) {
        if (!wanted.count(a.name)) continue;
        HumanBodyData::Clip c; c.name = a.name; c.tracks.resize(nb);
        for (const md::ogre::Track& tr : a.tracks) {
            const int bi = tr.bone < by_handle.size() ? by_handle[tr.bone] : -1;
            if (bi < 0 || tr.keyframes.empty()) continue;
            for (const md::ogre::Keyframe& k : tr.keyframes) {
                const double rot[4] = { k.rotation[0], k.rotation[1], k.rotation[2], k.rotation[3] }, bq[4] = { bind[bi][3], bind[bi][4], bind[bi][5], bind[bi][6] };
                double q[4]; QuatMul(bq, rot, q); QuatNorm(q);
                HumanBodyData::Key key{ k.time, (float)(bind[bi][0] + (double)k.translate[0] * kScale), (float)(bind[bi][1] + (double)k.translate[1] * kScale), (float)(bind[bi][2] + (double)k.translate[2] * kScale), (float)q[0], (float)q[1], (float)q[2], (float)q[3] };
                c.tracks[bi].push_back(key);
                if (k.time > c.duration) c.duration = k.time;
            }
        }
        o.clips.push_back(std::move(c));
    }
    // morphs: the mesh's face poses (first of a repeated name)
    std::set<std::string> seen;
    for (const md::ogre::Pose& po : m.poses) {
        if (!seen.insert(po.name).second) continue;
        std::vector<float> d((size_t)o.nv * 3, 0.f);
        for (const auto& of : po.offsets) if (of.first < o.nv) for (int k = 0; k < 3; ++k) d[of.first * 3 + k] = (float)((double)of.second[k] * kScale);
        o.morph_names.push_back(po.name); o.morph_delta.push_back(std::move(d));
    }
    return true;
}

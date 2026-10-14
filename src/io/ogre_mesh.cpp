// Behaviour ported from Project Okran src/io/ogre_mesh.rs (MIT, Copyright (c) 2026 brayniac6-glitch). See THIRD_PARTY.md.
#include <monkey_dust/io/ogre_mesh.h>
#include <cstdio>
#include <cstring>

namespace md::ogre {
namespace vtype {
size_t Size(uint16_t t) {
    switch (t) {
    case FLOAT1: case COLOUR: case UBYTE4: case COLOUR_ARGB: case COLOUR_ABGR: case SHORT2: return 4;
    case FLOAT2: case SHORT4: return 8;
    case FLOAT3: return 12;
    case FLOAT4: return 16;
    case SHORT1: return 2;
    case SHORT3: return 6;
    default: return 0;
    }
}
}

namespace {

using namespace mesh_chunk;

const uint16_t kMeshChildren[] = {GEOMETRY, SUBMESH, MESH_SKELETON_LINK, MESH_BONE_ASSIGNMENT, MESH_LOD, MESH_BOUNDS, SUBMESH_NAME_TABLE, 0xB000, MESH_POSES, 0xD000, 0xE000};
const uint16_t kSubmeshChildren[] = {GEOMETRY, SUBMESH_OPERATION, SUBMESH_BONE_ASSIGNMENT, SUBMESH_TEXTURE_ALIAS};

void Invalid(Reader& r, size_t at, const std::string& what) { r.Fail(ErrorKind::Invalid, at, what); }

BoneAssignment ReadBoneAssignment(Reader& r) {
    BoneAssignment b;
    b.vertex = r.U32("bone assignment vertex"); b.bone = r.U16("bone assignment bone"); b.weight = r.F32("bone assignment weight");
    return b;
}

void ReadGeometry(Reader& r, const Chunk& c, Geometry& g) {
    g.vertex_count = r.U32("vertex count");
    const uint16_t parts[] = {GEOMETRY_VERTEX_DECLARATION, GEOMETRY_VERTEX_BUFFER};
    Chunk sub;
    while (r.Child(parts, 2, sub)) {
        if (sub.id == GEOMETRY_VERTEX_DECLARATION) {
            Chunk e;
            while (r.Child(GEOMETRY_VERTEX_ELEMENT, e)) {
                VertexElement el;
                el.source = r.U16("element source"); el.type = r.U16("element type"); el.semantic = r.U16("element semantic");
                el.offset = r.U16("element offset"); el.index = r.U16("element index");
                g.elements.push_back(el);
            }
            continue;
        }
        VertexBuffer b;
        b.bind = r.U16("buffer bind"); b.stride = r.U16("buffer vertex size");
        Chunk d;
        if (!r.Child(GEOMETRY_VERTEX_BUFFER_DATA, d)) { Invalid(r, sub.start, "vertex buffer without data"); return; }
        size_t n = size_t(g.vertex_count) * b.stride;
        const uint8_t* p = r.Take(n, "vertex buffer data");
        if (!p) return;
        b.data.assign(p, p + n);
        g.buffers.push_back(std::move(b));
    }
    if (r.Failed()) return;
    for (const VertexElement& e : g.elements) {
        const VertexBuffer* b = nullptr;
        for (const VertexBuffer& x : g.buffers) if (x.bind == e.source) { b = &x; break; }
        if (!b) { Invalid(r, c.start, "element uses missing buffer " + std::to_string(e.source)); return; }
        size_t sz = vtype::Size(e.type);
        if (sz && size_t(e.offset) + sz > b->stride) { Invalid(r, c.start, "element at " + std::to_string(e.offset) + " overruns stride " + std::to_string(b->stride)); return; }
    }
}

void ReadSubmesh(Reader& r, SubMesh& s) {
    s.material = r.Line("submesh material");
    s.use_shared = r.U8("submesh use shared") != 0;
    size_t count = r.U32("index count");
    bool idx32 = r.U8("index 32-bit") != 0;
    const uint8_t* p = r.Take(count * (idx32 ? 4 : 2), "indices");
    if (!p) return;
    s.indices.resize(count);
    for (size_t i = 0; i < count; ++i) {
        if (idx32) s.indices[i] = uint32_t(p[4 * i]) | uint32_t(p[4 * i + 1]) << 8 | uint32_t(p[4 * i + 2]) << 16 | uint32_t(p[4 * i + 3]) << 24;
        else s.indices[i] = uint32_t(p[2 * i]) | uint32_t(p[2 * i + 1]) << 8;
    }
    Chunk sub;
    while (r.Child(kSubmeshChildren, 4, sub)) {
        switch (sub.id) {
        case GEOMETRY: s.has_geometry = true; ReadGeometry(r, sub, s.geometry); break;
        case SUBMESH_OPERATION: s.operation = r.U16("operation type"); break;
        case SUBMESH_BONE_ASSIGNMENT: s.bone_assignments.push_back(ReadBoneAssignment(r)); break;
        case SUBMESH_TEXTURE_ALIAS: { std::string a = r.Line("alias name"); std::string t = r.Line("alias texture"); s.texture_aliases.emplace_back(a, t); break; }
        default: r.Skip(sub);
        }
        if (r.Failed()) return;
    }
}

void ReadMeshChunk(Reader& r, const Chunk& c, Mesh& m) {
    m.skeletally_animated = r.U8("skeletally animated") != 0;
    Chunk sub;
    while (r.Child(kMeshChildren, sizeof kMeshChildren / sizeof *kMeshChildren, sub)) {
        switch (sub.id) {
        case GEOMETRY: m.has_shared = true; ReadGeometry(r, sub, m.shared); break;
        case SUBMESH: m.submeshes.emplace_back(); ReadSubmesh(r, m.submeshes.back()); break;
        case MESH_SKELETON_LINK: m.has_skeleton = true; m.skeleton = r.Line("skeleton name"); break;
        case MESH_BONE_ASSIGNMENT: m.bone_assignments.push_back(ReadBoneAssignment(r)); break;
        case MESH_BOUNDS:
            m.has_bounds = true; r.F32x3("bounds", m.bounds_min); r.F32x3("bounds", m.bounds_max); m.bounds_radius = r.F32("radius");
            break;
        default:
            if (sub.id == MESH_LOD && m.version.find("v1.100") == std::string::npos) {
                // v1.41 / v1.8 store a wrong MESH_LOD length: read its header fields and skip each LOD_USAGE by its (correct) length
                r.Line("lod strategy"); r.U16("lod level count"); r.U8("lod manual");
                Chunk u;
                while (r.Child(MESH_LOD_USAGE, u)) r.Skip(u);
            } else if (sub.id == SUBMESH_NAME_TABLE) {
                Chunk e;
                while (r.Child(SUBMESH_NAME_TABLE_ELEMENT, e)) { uint16_t i = r.U16("submesh index"); std::string n = r.Line("submesh name"); m.submesh_names.emplace_back(i, n); }
            } else if (sub.id == MESH_POSES) {
                bool normal_flag = m.version.find("v1.41") == std::string::npos;   // v1.8+ poses say whether their vertices carry normal offsets too
                Chunk pc;
                while (r.Child(MESH_POSE, pc)) {
                    Pose pose;
                    pose.name = r.Line("pose name"); pose.target = r.U16("pose target");
                    bool normals = normal_flag && r.U8("pose includes normals") != 0;
                    Chunk pv;
                    while (r.Child(MESH_POSE_VERTEX, pv)) {
                        uint32_t i = r.U32("pose vertex index");
                        std::array<float, 3> d; r.F32x3("pose vertex offset", d.data());
                        if (normals) { float nrm[3]; r.F32x3("pose vertex normal", nrm); }
                        pose.offsets.emplace_back(i, d);
                    }
                    m.poses.push_back(std::move(pose));
                }
            } else {
                r.Skip(sub);
            }
        }
        if (r.Failed()) return;
    }
    // every index must address a vertex of the geometry it draws from
    for (const SubMesh& s : m.submeshes) {
        uint32_t n = s.use_shared ? (m.has_shared ? m.shared.vertex_count : 0) : (s.has_geometry ? s.geometry.vertex_count : 0);
        for (uint32_t i : s.indices) if (i >= n) { Invalid(r, c.start, "submesh '" + s.material + "' index " + std::to_string(i) + " >= " + std::to_string(n) + " vertices"); return; }
    }
}

} // namespace

const VertexElement* Geometry::Element(uint16_t sem, uint16_t index) const {
    for (const VertexElement& e : elements) if (e.semantic == sem && e.index == index) return &e;
    return nullptr;
}

bool Geometry::Floats(uint16_t sem, uint16_t index, int n, std::vector<float>& out) const {
    const VertexElement* e = Element(sem, index);
    if (!e) return false;
    int comps = e->type == vtype::FLOAT1 ? 1 : e->type == vtype::FLOAT2 ? 2 : e->type == vtype::FLOAT3 ? 3 : e->type == vtype::FLOAT4 ? 4 : 0;
    if (!comps) return false;
    const VertexBuffer* b = nullptr;
    for (const VertexBuffer& x : buffers) if (x.bind == e->source) { b = &x; break; }
    if (!b) return false;
    out.assign(size_t(vertex_count) * n, 0.f);
    for (size_t v = 0; v < vertex_count; ++v)
        for (int c = 0; c < comps && c < n; ++c) std::memcpy(&out[v * n + c], &b->data[v * b->stride + e->offset + 4 * c], 4);
    return true;
}

bool Geometry::Colours(std::vector<float>& out) const {
    const VertexElement* e = Element(semantic::DIFFUSE, 0);
    if (!e) return false;
    bool bgra;
    if (e->type == vtype::COLOUR_ARGB || e->type == vtype::COLOUR) bgra = true;
    else if (e->type == vtype::COLOUR_ABGR) bgra = false;
    else return false;
    const VertexBuffer* b = nullptr;
    for (const VertexBuffer& x : buffers) if (x.bind == e->source) { b = &x; break; }
    if (!b) return false;
    out.assign(size_t(vertex_count) * 4, 0.f);
    for (size_t v = 0; v < vertex_count; ++v) {
        size_t at = v * b->stride + e->offset;
        if (at + 4 > b->data.size()) return false;
        const uint8_t* p = &b->data[at];
        float* o = &out[v * 4];
        if (bgra) { o[0] = p[2] / 255.f; o[1] = p[1] / 255.f; o[2] = p[0] / 255.f; o[3] = p[3] / 255.f; }
        else for (int k = 0; k < 4; ++k) o[k] = p[k] / 255.f;
    }
    return true;
}

bool Mesh::ApplyPose(const std::string& name, float weight) {
    const Pose* pose = nullptr;
    for (const Pose& p : poses) if (p.name == name) { pose = &p; break; }
    if (!pose) return false;
    Geometry* g = nullptr;
    if (pose->target == 0) g = has_shared ? &shared : nullptr;
    else if (size_t(pose->target) - 1 < submeshes.size() && submeshes[pose->target - 1].has_geometry) g = &submeshes[pose->target - 1].geometry;
    if (!g) return false;
    const VertexElement* e = g->Element(semantic::POSITION, 0);
    if (!e || e->type != vtype::FLOAT3) return false;
    VertexBuffer* b = nullptr;
    for (VertexBuffer& x : g->buffers) if (x.bind == e->source) { b = &x; break; }
    if (!b) return false;
    for (const auto& o : pose->offsets) {
        size_t at = size_t(o.first) * b->stride + e->offset;
        for (int k = 0; k < 3; ++k) {
            size_t p = at + size_t(k) * 4;
            if (p + 4 > b->data.size()) continue;
            float v; std::memcpy(&v, &b->data[p], 4);
            v += o.second[k] * weight;
            std::memcpy(&b->data[p], &v, 4);
        }
    }
    return true;
}

bool ParseMesh(const uint8_t* data, size_t size, Mesh& out, Error& err) {
    out = Mesh(); err = Error();
    Reader r(data, size);
    uint16_t id = r.U16("header");
    if (!r.Failed() && id != mesh_chunk::HEADER) { err.kind = ErrorKind::BadHeader; err.id = id; return false; }
    std::string version = r.Line("version");
    bool found = false;
    while (!r.Failed() && !r.AtEnd()) {
        Chunk c = r.Header();
        if (r.Failed()) break;
        if (c.id == mesh_chunk::MESH && !found) { found = true; out.version = version; ReadMeshChunk(r, c, out); }
        else r.Skip(c);
    }
    if (r.Failed()) { err = r.Err(); return false; }
    if (!found) { err.kind = ErrorKind::Invalid; err.what = "no mesh chunk"; return false; }
    return true;
}

bool ReadMesh(const std::string& path, Mesh& out, Error& err) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) { err = Error(); err.kind = ErrorKind::Io; err.what = path + ": cannot open"; return false; }
    std::vector<uint8_t> buf;
    uint8_t tmp[65536];
    for (size_t n; (n = std::fread(tmp, 1, sizeof tmp, f)) > 0;) buf.insert(buf.end(), tmp, tmp + n);
    std::fclose(f);
    return ParseMesh(buf.data(), buf.size(), out, err);
}

} // namespace md::ogre

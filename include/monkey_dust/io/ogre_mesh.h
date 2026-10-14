// Ogre binary `.mesh` files as shipped in Kenshi's `data/`. Every chunk is `u16 id, u32 length`; strings end in `\n`. Known chunks are read by content
// (v1.8 / v1.41 write wrong parent lengths), unknown ones are skipped by their length. Positions stay in the file's units (Kenshi: 0.1 m).
//
// This is a format reader only: it contains no game data. LOAD TIME only (std::string / std::vector).
//
// Behaviour ported from Project Okran (https://github.com/brayniac6-glitch/Project-Okran, MIT licence):
// src/io/ogre_mesh.rs. Copyright (c) 2026 brayniac6-glitch. See THIRD_PARTY.md for the licence text.
#pragma once
#include <monkey_dust/io/ogre_chunk.h>
#include <array>
#include <string>
#include <utility>
#include <vector>

namespace md::ogre {

namespace mesh_chunk {
constexpr uint16_t HEADER = 0x1000, MESH = 0x3000, SUBMESH = 0x4000, SUBMESH_OPERATION = 0x4010, SUBMESH_BONE_ASSIGNMENT = 0x4100,
    SUBMESH_TEXTURE_ALIAS = 0x4200, GEOMETRY = 0x5000, GEOMETRY_VERTEX_DECLARATION = 0x5100, GEOMETRY_VERTEX_ELEMENT = 0x5110,
    GEOMETRY_VERTEX_BUFFER = 0x5200, GEOMETRY_VERTEX_BUFFER_DATA = 0x5210, MESH_SKELETON_LINK = 0x6000, MESH_BONE_ASSIGNMENT = 0x7000,
    MESH_LOD = 0x8000, MESH_LOD_USAGE = 0x8100, MESH_BOUNDS = 0x9000, SUBMESH_NAME_TABLE = 0xA000, SUBMESH_NAME_TABLE_ELEMENT = 0xA100,
    MESH_POSES = 0xC000, MESH_POSE = 0xC100, MESH_POSE_VERTEX = 0xC111;
}
namespace semantic {
constexpr uint16_t POSITION = 1, BLEND_WEIGHTS = 2, BLEND_INDICES = 3, NORMAL = 4, DIFFUSE = 5, SPECULAR = 6, TEXTURE_COORDINATES = 7, BINORMAL = 8, TANGENT = 9;
}
namespace vtype {
constexpr uint16_t FLOAT1 = 0, FLOAT2 = 1, FLOAT3 = 2, FLOAT4 = 3, COLOUR = 4, SHORT1 = 5, SHORT2 = 6, SHORT3 = 7, SHORT4 = 8, UBYTE4 = 9, COLOUR_ARGB = 10, COLOUR_ABGR = 11;
// Byte size of an element of this type, 0 when unknown.
size_t Size(uint16_t t);
}
constexpr uint16_t OP_TRIANGLE_LIST = 4;   // the default when a submesh has no operation chunk

struct VertexElement { uint16_t source = 0, type = 0, semantic = 0, offset = 0, index = 0; };
struct VertexBuffer { uint16_t bind = 0, stride = 0; std::vector<uint8_t> data; };

struct Geometry {
    uint32_t vertex_count = 0;
    std::vector<VertexElement> elements;
    std::vector<VertexBuffer> buffers;
    const VertexElement* Element(uint16_t semantic, uint16_t index) const;
    // One float-typed element per vertex as N floats (missing components 0); false when absent or not a float type.
    bool Floats(uint16_t semantic, uint16_t index, int n, std::vector<float>& out) const;
    bool Positions(std::vector<float>& out) const { return Floats(semantic::POSITION, 0, 3, out); }
    bool Normals(std::vector<float>& out) const { return Floats(semantic::NORMAL, 0, 3, out); }
    bool Uvs(uint16_t set, std::vector<float>& out) const { return Floats(semantic::TEXTURE_COORDINATES, set, 2, out); }
    // Diffuse colours as RGBA 0..1: COLOUR_ARGB and COLOUR store B, G, R, A bytes; COLOUR_ABGR stores R, G, B, A.
    bool Colours(std::vector<float>& out) const;
};

struct BoneAssignment { uint32_t vertex = 0; uint16_t bone = 0; float weight = 0; };

struct SubMesh {
    std::string material;
    bool use_shared = false;
    std::vector<uint32_t> indices;
    bool has_geometry = false;     // own vertices; false when use_shared
    Geometry geometry;
    uint16_t operation = OP_TRIANGLE_LIST;
    std::vector<BoneAssignment> bone_assignments;
    std::vector<std::pair<std::string, std::string>> texture_aliases;
};

// A morph target: vertex offsets for one geometry.
struct Pose {
    std::string name;
    uint16_t target = 0;           // 0 = the shared geometry, n = submesh n - 1
    std::vector<std::pair<uint32_t, std::array<float, 3>>> offsets;
};

struct Mesh {
    std::string version;           // e.g. "MeshSerializer_v1.8"
    bool skeletally_animated = false;
    bool has_shared = false;
    Geometry shared;
    std::vector<SubMesh> submeshes;
    bool has_skeleton = false;
    std::string skeleton;
    bool has_bounds = false;
    float bounds_min[3] = {}, bounds_max[3] = {}, bounds_radius = 0;
    std::vector<BoneAssignment> bone_assignments;   // for the shared geometry
    std::vector<std::pair<uint16_t, std::string>> submesh_names;
    std::vector<Pose> poses;
    // vertex positions of pose `name` moved by weight * offsets (positions only, FLOAT3 only); false when the pose or its target is missing
    bool ApplyPose(const std::string& name, float weight);
};

// Parse a whole `.mesh`; every byte must belong to a chunk.
bool ParseMesh(const uint8_t* data, size_t size, Mesh& out, Error& err);
bool ReadMesh(const std::string& path, Mesh& out, Error& err);

} // namespace md::ogre

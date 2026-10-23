// The NPC body (what tools/md_convert.py + tools/morph_gen_b.py wrote into game/data/props/md_human.glb), built at run time from the Kenshi install: human_male.mesh + male_skeleton.skeleton.
// Mesh, skeleton and the 14 clips come from the install files; morphs are only the mesh's own face poses (1:1 with the original; no hand-made body morphs).
// LOAD TIME only (std::vector / std::string). Units: metres (the file's 0.1 m units x 0.1), column-major matrices.
#pragma once
#include <string>
#include <vector>
#include <cstdint>

struct HumanBodyData {
    uint32_t nv = 0;
    std::vector<float> pos, nrm, uv;          // 3 / 3 / 2 per vertex
    std::vector<uint8_t> joint;               // 4 per vertex
    std::vector<float> weight;                // 4 per vertex, normalised
    std::vector<uint32_t> idx;
    std::vector<std::string> bone_names;
    std::vector<int> parent;
    std::vector<float> bind_t, bind_q, inv_bind;   // 3 / 4 (xyzw) / 16 per bone
    struct Key { float t, tx, ty, tz, qx, qy, qz, qw; };
    struct Clip { std::string name; float duration = 0; std::vector<std::vector<Key>> tracks; };   // tracks[bone]
    std::vector<Clip> clips;
    std::vector<std::string> morph_names;     // face poses of the mesh
    std::vector<std::vector<float>> morph_delta;   // 3 floats per vertex
};

// The 14 clips md_human.glb holds, in the skeleton file's order.
bool BuildHumanBody(const std::string& kenshi_root, HumanBodyData& out);

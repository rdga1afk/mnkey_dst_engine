// Ogre binary `.skeleton` files as shipped in Kenshi's `data/`. Parent chunks (ANIMATION, ANIMATION_TRACK) are read by content like Ogre's own reader;
// stored lengths are only used to skip unknown chunks and for the "is the optional scale present" rule.
//
// This is a format reader only: it contains no game data. LOAD TIME only (std::string / std::vector).
//
// Behaviour ported from Project Okran (https://github.com/brayniac6-glitch/Project-Okran, MIT licence):
// src/io/ogre_skeleton.rs. Copyright (c) 2026 brayniac6-glitch. See THIRD_PARTY.md for the licence text.
#pragma once
#include <monkey_dust/io/ogre_chunk.h>
#include <string>
#include <utility>
#include <vector>

namespace md::ogre {

namespace skeleton_chunk {
constexpr uint16_t HEADER = 0x1000, BLENDMODE = 0x1010, BONE = 0x2000, BONE_PARENT = 0x3000, ANIMATION = 0x4000, ANIMATION_BASEINFO = 0x4010,
    ANIMATION_TRACK = 0x4100, ANIMATION_TRACK_KEYFRAME = 0x4110, ANIMATION_LINK = 0x5000;
}

struct Bone {
    std::string name;
    uint16_t handle = 0;
    float position[3] = {};
    float orientation[4] = {};         // quaternion as stored: x, y, z, w
    bool has_scale = false;
    float scale[3] = {};
    bool has_parent = false;
    uint16_t parent = 0;
};

struct Keyframe {
    float time = 0;
    float rotation[4] = {};            // x, y, z, w
    float translate[3] = {};
    bool has_scale = false;
    float scale[3] = {};
};

struct Track { uint16_t bone = 0; std::vector<Keyframe> keyframes; };

struct Animation {
    std::string name;
    float length = 0;
    bool has_base = false;             // additive animation
    std::string base_name;
    float base_time = 0;
    std::vector<Track> tracks;
};

struct Skeleton {
    std::string version;
    bool has_blend_mode = false;
    uint16_t blend_mode = 0;
    std::vector<Bone> bones;           // file order; look up by handle with Bone()
    std::vector<Animation> animations;
    std::vector<std::pair<std::string, float>> links;   // animations borrowed from another skeleton file, with a scale
    const Bone* FindBone(uint16_t handle) const;
};

// Parse a whole `.skeleton`; every byte must belong to a chunk.
bool ParseSkeleton(const uint8_t* data, size_t size, Skeleton& out, Error& err);
bool ReadSkeleton(const std::string& path, Skeleton& out, Error& err);

} // namespace md::ogre

// Behaviour ported from Project Okran src/io/ogre_skeleton.rs (MIT, Copyright (c) 2026 brayniac6-glitch). See THIRD_PARTY.md.
#include <monkey_dust/io/ogre_skeleton.h>
#include <cstdio>

namespace md::ogre {
namespace {

using namespace skeleton_chunk;
constexpr size_t kChunkHeader = 6;

void F32x4(Reader& r, const char* what, float out[4]) { for (int i = 0; i < 4; ++i) out[i] = r.F32(what); }

// The optional trailing scale is present only if the stored chunk length exceeds the size of the chunk without it.
bool HasScale(const Chunk& c, size_t size_without_scale) { return size_t(c.len) > size_without_scale; }

void ReadBone(Reader& r, const Chunk& c, Bone& b) {
    b.name = r.Line("bone name"); b.handle = r.U16("bone handle");
    r.F32x3("bone position", b.position); F32x4(r, "bone orientation", b.orientation);
    // the shipped files' writer leaves the name out of the stored length (36 = no scale, 48 = scale): compare against the size without the name
    if (HasScale(c, kChunkHeader + 2 + 12 + 16)) { b.has_scale = true; r.F32x3("bone scale", b.scale); }
}

void ReadAnimation(Reader& r, Animation& a) {
    a.name = r.Line("animation name"); a.length = r.F32("animation length");
    Chunk c;
    if (r.Child(ANIMATION_BASEINFO, c)) { a.has_base = true; a.base_name = r.Line("base animation"); a.base_time = r.F32("base key time"); }
    while (r.Child(ANIMATION_TRACK, c)) {
        Track t; t.bone = r.U16("track bone");
        Chunk k;
        while (r.Child(ANIMATION_TRACK_KEYFRAME, k)) {
            Keyframe kf;
            kf.time = r.F32("key time"); F32x4(r, "key rotation", kf.rotation); r.F32x3("key translate", kf.translate);
            if (HasScale(k, kChunkHeader + 4 + 16 + 12)) { kf.has_scale = true; r.F32x3("key scale", kf.scale); }
            t.keyframes.push_back(kf);
        }
        a.tracks.push_back(std::move(t));
    }
}

} // namespace

const Bone* Skeleton::FindBone(uint16_t handle) const {
    for (const Bone& b : bones) if (b.handle == handle) return &b;
    return nullptr;
}

bool ParseSkeleton(const uint8_t* data, size_t size, Skeleton& out, Error& err) {
    out = Skeleton(); err = Error();
    Reader r(data, size);
    uint16_t id = r.U16("header");
    if (!r.Failed() && id != HEADER) { err.kind = ErrorKind::BadHeader; err.id = id; return false; }
    out.version = r.Line("version");
    while (!r.Failed() && !r.AtEnd()) {
        Chunk c = r.Header();
        if (r.Failed()) break;
        switch (c.id) {
        case BLENDMODE: out.has_blend_mode = true; out.blend_mode = r.U16("blend mode"); break;
        case BONE: out.bones.emplace_back(); ReadBone(r, c, out.bones.back()); break;
        case BONE_PARENT: {
            uint16_t child = r.U16("parent child"), parent = r.U16("parent parent");
            if (r.Failed()) break;
            Bone* b = nullptr;
            for (Bone& x : out.bones) if (x.handle == child) { b = &x; break; }
            if (!b) { r.Fail(ErrorKind::Invalid, c.start, "parent link for unknown bone " + std::to_string(child)); break; }
            b->has_parent = true; b->parent = parent;
            break;
        }
        case ANIMATION: out.animations.emplace_back(); ReadAnimation(r, out.animations.back()); break;
        case ANIMATION_LINK: { std::string n = r.Line("linked skeleton"); float s = r.F32("link scale"); out.links.emplace_back(n, s); break; }
        default: r.Skip(c);
        }
    }
    if (r.Failed()) { err = r.Err(); return false; }
    // tracks must animate bones that exist
    for (const Animation& a : out.animations)
        for (const Track& t : a.tracks)
            if (!out.FindBone(t.bone)) { err.kind = ErrorKind::Invalid; err.at = 0; err.what = "animation '" + a.name + "' tracks unknown bone " + std::to_string(t.bone); return false; }
    return true;
}

bool ReadSkeleton(const std::string& path, Skeleton& out, Error& err) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) { err = Error(); err.kind = ErrorKind::Io; err.what = path + ": cannot open"; return false; }
    std::vector<uint8_t> buf;
    uint8_t tmp[65536];
    for (size_t n; (n = std::fread(tmp, 1, sizeof tmp, f)) > 0;) buf.insert(buf.end(), tmp, tmp + n);
    std::fclose(f);
    return ParseSkeleton(buf.data(), buf.size(), out, err);
}

} // namespace md::ogre

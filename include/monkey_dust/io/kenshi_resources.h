// Ogre-style resource lookup for a Kenshi install: Ogre finds meshes, skeletons and textures by file name anywhere in its resource folders, so paths stored
// in game data may be stale (e.g. `character/skins/hu/heads/x.dds` when the file is in `character/skins/human_male/`). A stored path is tried first,
// otherwise the file name is looked up, case-insensitively, among the files under `<root>/data` (the first path in sorted order wins).
//
// LOAD TIME only (std::string / std::unordered_map).
//
// Behaviour ported from Project Okran (https://github.com/brayniac6-glitch/Project-Okran, MIT licence):
// src/io/resources.rs (`normalize`, `index`, `find`; the enabled mods' own files are not indexed here). Copyright (c) 2026 brayniac6-glitch.
// See THIRD_PARTY.md for the licence text.
#pragma once
#include <string>
#include <unordered_map>

namespace md {

// A game-data path (`.\data\x\y.dds`) -> relative path with forward slashes (`data/x/y.dds`).
std::string NormalizeKenshiPath(const std::string& stored);

class KenshiResources {
public:
    // Index every file under `<root>/data`.
    void Index(const std::string& root);
    size_t Size() const { return by_name_.size(); }
    // A game-data path or a bare file name -> path relative to the install root (forward slashes) if the file exists, else "".
    std::string Find(const std::string& stored) const;
    // Same, joined to the install root.
    std::string Path(const std::string& stored) const;
    // Find(), and for a `_HI` texture the install lacks (the high resolution set is not shipped for every texture) the plain one. Okran marks this an assumption.
    std::string FindTexture(const std::string& stored) const;
    const std::string& Root() const { return root_; }

private:
    std::string root_;
    std::unordered_map<std::string, std::string> by_name_;   // lower-case file name -> path relative to root_
};

} // namespace md

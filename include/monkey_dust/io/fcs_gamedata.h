// Which FCS files a game loads and in what order (mods.cfg, core files, mods/ and Steam Workshop folders), and the merged
// record set built from them. Load time only (std::string / std::map / std::vector on purpose).
//
// Behaviour ported from Project Okran (MIT, Copyright (c) 2026 brayniac6-glitch): src/io/mods.rs (load order,
// `parse_mods_cfg`, `find_mod`) and src/data/gamedata.rs (record merging). See THIRD_PARTY.md.
//
// Merge rules (Okran's F041671 spec):
//  - records merge by `string_id`; an unknown id creates a record tagged with the file's index (a warning when its
//    datatype says it modifies an existing record, bit 0);
//  - datatype bit 1 on an existing record replaces its name;
//  - bool/float/int/vec3/vec4/string/file fields overwrite per key;
//  - a reference whose three values are all INT32_MAX removes `category -> target`, any other sets it;
//  - an instance with an empty target removes the instance with that id, any other sets it;
//  - after a record is applied, a true bool field `REMOVED` deletes it;
//  - the id counter is the max over all files.
#pragma once
#include <monkey_dust/io/fcs.h>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

namespace md::fcs {

constexpr const char* CORE_BASE = "gamedata.base";
constexpr const char* STEAM_APP_ID = "233860"; // Kenshi: workshop mods live in steamapps/workshop/content/<id>/<item>/

// `data/mods.cfg` text -> mod names in order: blank lines and lines starting with `#`, `/` or `;` are skipped, only
// `*.mod` entries count, the extension is dropped, repeats are ignored.
std::vector<std::string> ParseModsCfg(const std::string& text);

struct LoadEntry {
    std::string name; // file name, e.g. "rebirth.mod"
    std::string path;
    bool core = false;
};

struct LoadOrder {
    std::vector<LoadEntry> files;
    std::vector<std::string> missing; // names from mods.cfg with no matching installed mod
};

// A Kenshi install: the game folder (the one containing `data/`), plus the Steam workshop folder when it exists.
struct Install {
    std::string root;
    std::string workshop; // empty when absent

    explicit Install(const std::string& game_root);

    std::string Data() const { return root + "/data"; }
    // `data/gamedata.base` exists. With require_exe also a `kenshi*.exe` and `data/gui` (a full game folder).
    bool Check(std::string& why, bool require_exe = true) const;
    // `gamedata.base`, then every `data/*.mod`, each after its header dependencies (ties by name).
    bool CoreFiles(std::vector<LoadEntry>& out, Error& err) const;
    // `mods/<name>/<name>.mod`, then any workshop item. Empty when not installed.
    std::string FindMod(const std::string& name) const;
    std::string ModsCfgText() const; // the install's data/mods.cfg ("" when absent)
    // The full order: core files, then `data/mods.cfg` mods (core names there are ignored).
    bool LoadOrderFor(LoadOrder& out, Error& err) const;
};

struct GameData {
    int32_t kind = 0;
    std::string name, string_id;
    size_t source = 0; // index into GameDataContainer::files of the file that created the record
    std::map<std::string, bool> bools;
    std::map<std::string, float> floats;
    std::map<std::string, int32_t> ints;
    std::map<std::string, std::array<float, 3>> vec3s;
    std::map<std::string, std::array<float, 4>> vec4s;
    std::map<std::string, std::string> strings;
    std::map<std::string, std::string> files;
    // category -> (target string_id, values), in the order first set
    std::vector<std::pair<std::string, std::vector<std::pair<std::string, std::array<int32_t, 3>>>>> references;
    std::vector<Instance> instances; // by id, in the order first set

    const std::vector<std::pair<std::string, std::array<int32_t, 3>>>* Refs(const std::string& category) const;
};

class GameDataContainer {
public:
    std::unordered_map<std::string, GameData> records;
    std::vector<std::string> order; // record ids in the order first loaded
    int32_t id_counter = 0;
    std::vector<std::string> files; // file names in the order applied
    std::vector<std::string> warnings;

    // Apply one parsed file on top of what is loaded.
    void ApplyFile(const std::string& name, const File& file);
    // Read and apply every file of a load order. A core file that fails is an error; a failing mod is skipped with a
    // warning (the original collects them into one dialog).
    bool Load(const LoadOrder& order, Error& err);
    const GameData* Get(const std::string& string_id) const;
    std::vector<const GameData*> OfType(int32_t kind) const;
};

} // namespace md::fcs

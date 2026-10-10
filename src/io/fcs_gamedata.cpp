// Load order and record merging. Behaviour ported from Project Okran src/io/mods.rs and src/data/gamedata.rs (MIT,
// Copyright (c) 2026 brayniac6-glitch), see THIRD_PARTY.md.
#include <monkey_dust/io/fcs_gamedata.h>
#include <algorithm>
#include <cctype>
#include <climits>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

namespace md::fcs {

namespace {
bool EqNoCase(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) if (std::tolower((unsigned char)a[i]) != std::tolower((unsigned char)b[i])) return false;
    return true;
}
std::string Trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace((unsigned char)s[a])) ++a;
    while (b > a && std::isspace((unsigned char)s[b - 1])) --b;
    return s.substr(a, b - a);
}
bool IsFile(const std::string& p) { std::error_code ec; return fs::is_regular_file(p, ec); }
bool IsDir(const std::string& p) { std::error_code ec; return fs::is_directory(p, ec); }
} // namespace

std::vector<std::string> ParseModsCfg(const std::string& text) {
    std::vector<std::string> out;
    std::istringstream in(text);
    std::string raw;
    while (std::getline(in, raw)) {
        std::string line = Trim(raw);
        if (line.empty() || line[0] == '#' || line[0] == '/' || line[0] == ';') continue;
        size_t dot = line.rfind('.');
        if (dot == std::string::npos) continue;
        std::string name = line.substr(0, dot), ext = line.substr(dot + 1);
        if (EqNoCase(ext, "mod") && !name.empty() && std::find(out.begin(), out.end(), name) == out.end()) out.push_back(name);
    }
    return out;
}

Install::Install(const std::string& game_root) : root(game_root) {
    while (root.size() > 1 && (root.back() == '/' || root.back() == '\\')) root.pop_back();
    // steamapps/common/<game> -> steamapps/workshop/content/<app>
    fs::path p(root);
    if (p.has_parent_path() && p.parent_path().has_parent_path()) {
        fs::path w = p.parent_path().parent_path() / "workshop" / "content" / STEAM_APP_ID;
        if (IsDir(w.string())) workshop = w.string();
    }
}

bool Install::Check(std::string& why, bool require_exe) const {
    if (!IsFile(Data() + "/" + CORE_BASE)) { why = root + " has no Kenshi game data (data/gamedata.base)"; return false; }
    if (!require_exe) return true;
    if (!IsDir(Data() + "/gui")) { why = root + " has no data/gui"; return false; }
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(root, ec)) {
        std::string n = e.path().filename().string();
        std::transform(n.begin(), n.end(), n.begin(), [](unsigned char c) { return (char)std::tolower(c); });
        if (n.rfind("kenshi", 0) == 0 && n.size() > 4 && n.compare(n.size() - 4, 4, ".exe") == 0) return true;
    }
    why = root + " has no Kenshi game (kenshi_x64.exe)";
    return false;
}

bool Install::CoreFiles(std::vector<LoadEntry>& out, Error& err) const {
    out.clear();
    std::string base = Data() + "/" + CORE_BASE;
    if (!IsFile(base)) { err = Error{}; err.kind = ErrorKind::Io; err.what = base + " not found"; return false; }
    struct M { std::string path; std::vector<std::string> deps; };
    std::map<std::string, M> mods; // sorted by file name (byte order), like Okran's BTreeMap
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(Data(), ec)) {
        if (!e.is_regular_file()) continue;
        std::string file = e.path().filename().string();
        if (file.size() < 4 || !EqNoCase(file.substr(file.size() - 4), ".mod")) continue;
        Header h; Error he;
        if (!ReadHeader(e.path().string(), h, he)) { err = he; return false; }
        mods[file] = M{e.path().string(), FileList(h.dependencies)};
    }
    out.push_back(LoadEntry{CORE_BASE, base, true});
    std::vector<std::string> done{CORE_BASE};
    while (!mods.empty()) {
        // first by name whose core dependencies are all loaded; break cycles by name
        auto ready = mods.end();
        for (auto it = mods.begin(); it != mods.end(); ++it) {
            bool ok = true;
            for (const auto& d : it->second.deps) if (std::find(done.begin(), done.end(), d) == done.end() && mods.count(d)) { ok = false; break; }
            if (ok) { ready = it; break; }
        }
        if (ready == mods.end()) ready = mods.begin();
        out.push_back(LoadEntry{ready->first, ready->second.path, true});
        done.push_back(ready->first);
        mods.erase(ready);
    }
    return true;
}

std::string Install::FindMod(const std::string& name) const {
    std::string file = name + ".mod";
    std::string local = root + "/mods/" + name + "/" + file;
    if (IsFile(local)) return local;
    if (workshop.empty()) return "";
    std::vector<std::string> items;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(workshop, ec)) items.push_back(e.path().string());
    std::sort(items.begin(), items.end());
    for (const auto& d : items) { std::string p = d + "/" + file; if (IsFile(p)) return p; }
    return "";
}

std::string Install::ModsCfgText() const {
    std::ifstream f(Data() + "/mods.cfg", std::ios::binary);
    if (!f) return "";
    std::stringstream ss; ss << f.rdbuf();
    return ss.str();
}

bool Install::LoadOrderFor(LoadOrder& out, Error& err) const {
    out = LoadOrder{};
    if (!CoreFiles(out.files, err)) return false;
    for (const std::string& name : ParseModsCfg(ModsCfgText())) {
        std::string file = name + ".mod";
        bool dup = false;
        for (const auto& f : out.files) if (EqNoCase(f.name, file)) { dup = true; break; }
        if (dup) continue;
        std::string path = FindMod(name);
        if (path.empty()) out.missing.push_back(name);
        else out.files.push_back(LoadEntry{file, path, false});
    }
    return true;
}

const std::vector<std::pair<std::string, std::array<int32_t, 3>>>* GameData::Refs(const std::string& category) const {
    for (const auto& c : references) if (c.first == category) return &c.second;
    return nullptr;
}

namespace {
constexpr std::array<int32_t, 3> REF_REMOVE = {INT32_MAX, INT32_MAX, INT32_MAX};

void SetRef(GameData& g, const std::string& category, const std::string& target, const std::array<int32_t, 3>& values) {
    size_t ci = g.references.size();
    for (size_t i = 0; i < g.references.size(); ++i) if (g.references[i].first == category) { ci = i; break; }
    if (ci == g.references.size()) {
        if (values == REF_REMOVE) return;
        g.references.emplace_back(category, std::vector<std::pair<std::string, std::array<int32_t, 3>>>{});
    }
    auto& list = g.references[ci].second;
    size_t at = list.size();
    for (size_t i = 0; i < list.size(); ++i) if (list[i].first == target) { at = i; break; }
    bool remove = values == REF_REMOVE;
    if (at < list.size()) { if (remove) list.erase(list.begin() + at); else list[at].second = values; }
    else if (!remove) list.emplace_back(target, values);
}

void Apply(GameData& g, const Record& r) {
    for (const auto& kv : r.bools) g.bools[kv.first] = kv.second;
    for (const auto& kv : r.floats) g.floats[kv.first] = kv.second;
    for (const auto& kv : r.ints) g.ints[kv.first] = kv.second;
    for (const auto& kv : r.vec3s) g.vec3s[kv.first] = kv.second;
    for (const auto& kv : r.vec4s) g.vec4s[kv.first] = kv.second;
    for (const auto& kv : r.strings) g.strings[kv.first] = kv.second;
    for (const auto& kv : r.files) g.files[kv.first] = kv.second;
    for (const auto& cat : r.references)
        for (const auto& rf : cat.second) SetRef(g, cat.first, rf.target, {rf.values[0], rf.values[1], rf.values[2]});
    for (const Instance& inst : r.instances) {
        size_t at = g.instances.size();
        for (size_t i = 0; i < g.instances.size(); ++i) if (g.instances[i].id == inst.id) { at = i; break; }
        bool has = at < g.instances.size();
        if (inst.target.empty()) { if (has) g.instances.erase(g.instances.begin() + at); }
        else if (has) g.instances[at] = inst;
        else g.instances.push_back(inst);
    }
}
} // namespace

void GameDataContainer::ApplyFile(const std::string& name, const File& file) {
    size_t source = files.size();
    files.push_back(name);
    id_counter = std::max(id_counter, file.header.id_counter);
    for (const Record& r : file.records) {
        uint32_t flags = r.datatype & 0x7fffffffu;
        auto it = records.find(r.string_id);
        GameData* rec;
        if (it != records.end()) {
            if (flags & DATATYPE_SETS_NAME) it->second.name = r.name;
            rec = &it->second;
        } else {
            if (flags & DATATYPE_MODIFIES)
                warnings.push_back("[Mods] Item " + r.name + " (" + r.string_id + ") modified by " + name + " does not exist");
            order.push_back(r.string_id);
            GameData g; g.kind = r.kind; g.name = r.name; g.string_id = r.string_id; g.source = source;
            rec = &records.emplace(r.string_id, std::move(g)).first->second;
        }
        Apply(*rec, r);
        auto removed = rec->bools.find("REMOVED");
        if (removed != rec->bools.end() && removed->second) {
            records.erase(r.string_id);
            order.erase(std::remove(order.begin(), order.end(), r.string_id), order.end());
        }
    }
}

bool GameDataContainer::Load(const LoadOrder& lo, Error& err) {
    for (const auto& n : lo.missing) warnings.push_back("[Mods] Mod '" + n + "' not found");
    for (const LoadEntry& f : lo.files) {
        File file; Error e;
        if (ReadFile(f.path, file, e)) ApplyFile(f.name, file);
        else if (f.core) { err = e; return false; }
        else warnings.push_back("[Mods] Failed to load mod: " + f.name + " (" + e.Message() + ")");
    }
    return true;
}

const GameData* GameDataContainer::Get(const std::string& string_id) const {
    auto it = records.find(string_id);
    return it == records.end() ? nullptr : &it->second;
}

std::vector<const GameData*> GameDataContainer::OfType(int32_t kind) const {
    std::vector<const GameData*> out;
    for (const auto& id : order) { auto it = records.find(id); if (it != records.end() && it->second.kind == kind) out.push_back(&it->second); }
    return out;
}

} // namespace md::fcs

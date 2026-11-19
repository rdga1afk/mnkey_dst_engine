#include <monkey_dust/world/kenshi_save.h>
#include <monkey_dust/io/fcs.h>
#include <filesystem>
#include <map>

namespace md::save {
namespace {
namespace fs = std::filesystem;
using namespace md::fcs;
constexpr int K_STATS = 25, K_INSTANCES = 30, K_PLATOON = 34, K_CHARACTER_STATE = 36, K_MEDICAL = 57, K_CAMERA = 56;

const int32_t* FindInt(const Record& r, const char* k) { for (const auto& kv : r.ints) if (kv.first == k) return &kv.second; return nullptr; }
const float* FindFloat(const Record& r, const char* k) { for (const auto& kv : r.floats) if (kv.first == k) return &kv.second; return nullptr; }
const std::string* FindStr(const Record& r, const char* k) { for (const auto& kv : r.strings) if (kv.first == k) return &kv.second; return nullptr; }
bool FindBool(const Record& r, const char* k) { for (const auto& kv : r.bools) if (kv.first == k && kv.second) return true; return false; }
const std::array<float, 3>* FindVec3(const Record& r, const char* k) { for (const auto& kv : r.vec3s) if (kv.first == k) return &kv.second; return nullptr; }

bool Load(const fs::path& p, File& f, std::string& err) {
    Error e;
    std::error_code ec;
    if (!fs::exists(p, ec)) { err = p.string() + ": file not found"; return false; }
    if (!ReadFile(p.string(), f, e)) { err = p.string() + ": " + e.Message(); return false; }
    if (f.header.filetype != FILETYPE_V15) { err = p.string() + ": not a Kenshi save file (filetype " + std::to_string(f.header.filetype) + ", expected 15)"; return false; }
    return true;
}

Record Rec(int kind, int id, const std::string& name, const std::string& sid) { Record r; r.kind = kind; r.id = id; r.name = name; r.string_id = sid; return r; }
}  // namespace

bool ReadKenshiSave(const std::string& dir, KenshiSave& out, std::string& err) {
    out = KenshiSave{};
    File quick;
    if (!Load(fs::path(dir) / "quick.save", quick, err)) return false;
    const Record* cam = nullptr;
    for (const Record& r : quick.records) if (r.kind == K_CAMERA) { cam = &r; break; }
    if (!cam) { err = dir + "/quick.save: no camera record (not a save, or damaged)"; return false; }
    if (const int32_t* v = FindInt(*cam, "time day")) out.day = *v;
    if (const int32_t* v = FindInt(*cam, "time hour")) out.hour = *v;
    if (const int32_t* v = FindInt(*cam, "time minute")) out.minute = *v;
    if (const int32_t* v = FindInt(*cam, "player money")) out.money = *v;
    if (const auto* v = FindVec3(*cam, "pos")) for (int i = 0; i < 3; ++i) out.camera[i] = (*v)[i];
    if (const std::string* v = FindStr(*cam, "pfaction name")) out.faction_name = *v;
    if (const std::string* v = FindStr(*cam, "version")) out.version = *v;
    for (const auto& cat : cam->references) if (cat.first == "zones") for (const Reference& z : cat.second) out.zones.emplace_back(z.values[0], z.values[1]);

    for (const Record& p : quick.records) {
        if (p.kind != K_PLATOON) continue;
        const std::string* fac = FindStr(p, "faction name");
        if (!fac || *fac != out.faction_name) continue;   // another faction's squad
        KSquad sq;
        const std::string* sid = FindStr(p, "platoon stringID");
        sq.id = sid ? *sid : p.name;
        if (const std::string* v = FindStr(p, "faction stringID")) sq.faction = *v;
        if (const auto* v = FindVec3(p, "position")) for (int i = 0; i < 3; ++i) sq.position[i] = (*v)[i];
        File pf;
        if (!Load(fs::path(dir) / "platoon" / (sq.id + ".platoon"), pf, err)) return false;
        std::map<std::string, const Record*> by_id;
        for (const Record& r : pf.records) by_id[r.string_id] = &r;
        for (const Record& coll : pf.records) {
            if (coll.kind != K_INSTANCES) continue;
            for (const Instance& in : coll.instances) {
                KChar c; c.template_id = in.target;
                for (int i = 0; i < 3; ++i) c.position[i] = in.position[i];
                for (int i = 0; i < 4; ++i) c.rotation[i] = in.rotation[i];
                for (const std::string& st : in.states) {
                    auto it = by_id.find(st); if (it == by_id.end()) continue;
                    const Record& s = *it->second;
                    if (s.kind == K_CHARACTER_STATE) { if (const std::string* n = FindStr(s, "name")) c.name = *n; }
                    else if (s.kind == K_STATS) c.stats = s.floats;
                    else if (s.kind == K_MEDICAL) {
                        c.dead = FindBool(s, "dead"); c.unconscious = FindBool(s, "unconcious");
                        if (const float* v = FindFloat(s, "blood")) c.blood = *v;
                        if (const float* v = FindFloat(s, "hung")) c.hunger = *v;
                        if (const float* v = FindFloat(s, "fed")) c.fed = *v;
                    }
                }
                sq.characters.push_back(std::move(c));
            }
        }
        out.squads.push_back(std::move(sq));
    }
    return true;
}

bool WriteKenshiSave(const std::string& dir, const KenshiSave& s, std::string& err) {
    std::error_code ec;
    fs::create_directories(fs::path(dir) / "platoon", ec);
    if (ec) { err = dir + ": " + ec.message(); return false; }
    Error e;
    File quick; quick.header.filetype = FILETYPE_V15;
    Record cam = Rec(K_CAMERA, 1, "camera", "1-quick-INGAME");
    cam.ints = { {"time day", s.day}, {"time hour", s.hour}, {"time minute", s.minute}, {"player money", s.money} };
    cam.vec3s = { {"pos", {s.camera[0], s.camera[1], s.camera[2]}} };
    cam.strings = { {"pfaction name", s.faction_name}, {"version", s.version} };
    std::vector<Reference> zr; for (const auto& z : s.zones) { Reference r; r.values[0] = z.first; r.values[1] = z.second; zr.push_back(r); }
    cam.references = { {"zones", zr} };
    quick.records.push_back(cam);
    int next = 2;
    for (const KSquad& sq : s.squads) {
        Record p = Rec(K_PLATOON, next, sq.id, std::to_string(next) + "-quick-INGAME"); ++next;
        p.strings = { {"platoon stringID", sq.id}, {"faction name", s.faction_name}, {"faction stringID", sq.faction} };
        p.vec3s = { {"position", {sq.position[0], sq.position[1], sq.position[2]}} };
        quick.records.push_back(p);
        File pf; pf.header.filetype = FILETYPE_V15;
        Record coll = Rec(K_INSTANCES, 1, "characters", "1-" + sq.id + "-INGAME");
        int id = 2;
        for (const KChar& c : sq.characters) {
            Instance in; in.id = std::to_string(id) + "-" + sq.id; in.target = c.template_id;
            for (int i = 0; i < 3; ++i) in.position[i] = c.position[i];
            for (int i = 0; i < 4; ++i) in.rotation[i] = c.rotation[i];
            auto add = [&](Record r) { in.states.push_back(r.string_id); pf.records.push_back(std::move(r)); };
            auto mk = [&](int kind) { const int i = id++; return Rec(kind, i, c.name, std::to_string(i) + "-" + sq.id); };
            Record g = mk(K_CHARACTER_STATE); g.strings = { {"name", c.name} }; add(g);
            Record st = mk(K_STATS); st.floats = c.stats; add(st);
            Record m = mk(K_MEDICAL);
            m.bools = { {"dead", c.dead}, {"unconcious", c.unconscious} };
            m.floats = { {"blood", c.blood}, {"hung", c.hunger}, {"fed", c.fed} }; add(m);
            coll.instances.push_back(std::move(in));
        }
        pf.records.insert(pf.records.begin(), coll);
        pf.header.id_counter = id;
        if (!WriteFile((fs::path(dir) / "platoon" / (sq.id + ".platoon")).string(), pf, e)) { err = e.Message(); return false; }
    }
    quick.header.id_counter = next;
    if (!WriteFile((fs::path(dir) / "quick.save").string(), quick, e)) { err = e.Message(); return false; }
    return true;
}

}  // namespace md::save

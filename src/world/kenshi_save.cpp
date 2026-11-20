#include <monkey_dust/world/kenshi_save.h>
#include <monkey_dust/io/fcs.h>
#include <filesystem>
#include <algorithm>
#include <cstring>
#include <map>

namespace md::save {
namespace {
namespace fs = std::filesystem;
constexpr int K_STATS = 25, K_INSTANCES = 30, K_PLATOON = 34, K_CHARACTER_STATE = 36, K_MEDICAL = 57, K_CAMERA = 56;

const int32_t* FindInt(const md::fcs::Record& r, const char* k) { for (const auto& kv : r.ints) if (kv.first == k) return &kv.second; return nullptr; }
const float* FindFloat(const md::fcs::Record& r, const char* k) { for (const auto& kv : r.floats) if (kv.first == k) return &kv.second; return nullptr; }
const std::string* FindStr(const md::fcs::Record& r, const char* k) { for (const auto& kv : r.strings) if (kv.first == k) return &kv.second; return nullptr; }
bool FindBool(const md::fcs::Record& r, const char* k) { for (const auto& kv : r.bools) if (kv.first == k && kv.second) return true; return false; }
bool HasBool(const md::fcs::Record& r, const char* k) { for (const auto& kv : r.bools) if (kv.first == k) return true; return false; }
const std::array<float, 3>* FindVec3(const md::fcs::Record& r, const char* k) { for (const auto& kv : r.vec3s) if (kv.first == k) return &kv.second; return nullptr; }

bool Load(const fs::path& p, md::fcs::File& f, std::string& err) {
    md::fcs::Error e;
    std::error_code ec;
    if (!fs::exists(p, ec)) { err = p.string() + ": file not found"; return false; }
    if (!md::fcs::ReadFile(p.string(), f, e)) { err = p.string() + ": " + e.Message(); return false; }
    if (f.header.filetype != md::fcs::FILETYPE_V15) { err = p.string() + ": not a Kenshi save file (filetype " + std::to_string(f.header.filetype) + ", expected 15)"; return false; }
    return true;
}

// the eleven order bools in the exe's source byte order 0..10 (0x1403eb710 / 0x1403ebf50); the writer visits them in a different order, see WriteKenshiSave
struct OrderBool { const char* key; bool KOrders::* field; };
const OrderBool ORDER_BOOLS[] = { {"heal", &KOrders::heal}, {"help", &KOrders::help}, {"rescue", &KOrders::rescue}, {"stay", &KOrders::stay}, {"feed", &KOrders::feed}, {"share", &KOrders::share},
                                  {"sleep", &KOrders::sleep}, {"ditch", &KOrders::ditch}, {"sit", &KOrders::sit}, {"eject", &KOrders::eject}, {"shootfirst", &KOrders::shootfirst} };

md::fcs::Record Rec(int kind, int id, const std::string& name, const std::string& sid) { md::fcs::Record r; r.kind = kind; r.id = id; r.name = name; r.string_id = sid; return r; }
}  // namespace

const std::vector<std::string>& StatKeys() {
    static const std::vector<std::string> k = {
        "strength", "endurance", "dexterity", "perception", "toughness2", "warrior spirit", "attack", "defence", "dodge", "ff", "athletics", "medic", "mass combat",
        "arrow defence", "stealth", "swimming", "thievery", "lockpicking", "bluff", "assassin", "survival", "climbing", "tracking", "doctor", "engineer", "weapon smith",
        "armour smith", "robotics", "bow smith", "science", "labouring", "farming", "cooking", "katana", "poles", "sabres", "hackers", "heavy weapons", "blunt", "turrets",
        "bow", "unarmed", "xp", "free attribute points" };
    return k;
}

float StatValue(const KChar& c, const std::string& key) {
    auto raw = [&](const char* k, bool& found) { for (const auto& kv : c.stats) if (kv.first == k) { found = true; return kv.second; } found = false; return 0.f; };
    bool f;
    if (key == "toughness2") { float v = raw("toughness2", f); return f ? v : raw("defence", f); }
    if (key == "lockpicking") { float v = raw("lockpicking", f); return f ? v : raw("thievery", f) * 0.5f; }
    return raw(key.c_str(), f);
}

bool ReadKenshiSave(const std::string& dir, KenshiSave& out, std::string& err) {
    out = KenshiSave{};
    md::fcs::File quick;
    if (!Load(fs::path(dir) / "quick.save", quick, err)) return false;
    const md::fcs::Record* cam = nullptr;
    for (const md::fcs::Record& r : quick.records) if (r.kind == K_CAMERA) { cam = &r; break; }
    if (!cam) { err = dir + "/quick.save: no camera record (not a save, or damaged)"; return false; }
    // a missing int reads 0 in the exe (its map lookup inserts a zero, 0x14047e390), so the defaults of the struct do not apply to a loaded save
    auto int_or0 = [&](const char* k) { const int32_t* v = FindInt(*cam, k); return v ? (int)*v : 0; };
    // 0x14066db50 clamps day / hour / minute at 0 from below
    out.day = std::max(0, int_or0("time day")); out.hour = std::max(0, int_or0("time hour")); out.minute = std::max(0, int_or0("time minute")); out.money = int_or0("player money");
    if (const auto* v = FindVec3(*cam, "pos")) for (int i = 0; i < 3; ++i) out.camera[i] = (*v)[i];
    if (const std::string* v = FindStr(*cam, "pfaction name")) out.faction_name = *v;
    if (const std::string* v = FindStr(*cam, "version")) out.version = *v;
    // 0x1403ebf50: the toggles are read only when the bool table has "ditch"; then every key is read (a missing one reads false, the exe's map inserts a zero)
    if (HasBool(*cam, "ditch")) {
        out.orders.present = true;
        for (const OrderBool& b : ORDER_BOOLS) out.orders.*b.field = FindBool(*cam, b.key);
        out.orders.formation = int_or0("formation");
    }
    // 0x14066db50: with no float `sky update current time` the defaults of KSky stay; otherwise all sky keys are read (missing ones read 0) and a saved
    // `sky updating` = false is replaced by updating = true with both speeds and both times 0
    if (FindFloat(*cam, "sky update current time")) {
        KSky& k = out.sky; k.present = true;
        auto f0 = [&](const char* key) { const float* v = FindFloat(*cam, key); return v ? *v : 0.f; };
        auto v0 = [&](const char* key, float* dst) { const auto* v = FindVec3(*cam, key); for (int i = 0; i < 3; ++i) dst[i] = v ? (*v)[i] : 0.f; };
        k.current_time = f0("sky update current time"); k.total_time = f0("sky update total time"); k.updating = FindBool(*cam, "sky updating");
        v0("sky ambient color mult", k.ambient); v0("sky ambient color mult speed", k.ambient_speed);
        k.clouds_density = f0("sky clouds density"); k.clouds_speed = f0("sky clouds density speed");
        if (!k.updating) { k.updating = true; k.clouds_speed = 0.f; for (float& a : k.ambient_speed) a = 0.f; k.current_time = k.total_time = 0.f; }
    }
    // 0x1402df0c0: prosperity when its float is there; then entries i = 0.. while the string `sq<i>` exists; the references of an entry j = 0.. while `<i>:sq list<j>I` exists
    if (const float* v = FindFloat(*cam, "prosperity")) { out.squad_list.has_prosperity = true; out.squad_list.prosperity = *v; }
    for (int i = 0;; ++i) {
        const std::string n = std::to_string(i);
        const std::string* nm = FindStr(*cam, ("sq" + n).c_str());
        if (!nm) break;
        KSquadListEntry e; e.name = *nm;
        if (const float* t = FindFloat(*cam, ("sqtime" + n).c_str())) e.time = *t;
        for (int j = 0; FindInt(*cam, (n + ":sq list" + std::to_string(j) + "I").c_str()); ++j) {
            const std::string pre = n + ":sq list" + std::to_string(j);
            KSquadRef r; r.type = int_or0((pre + "TYPE").c_str()); r.c = int_or0((pre + "C").c_str()); r.i = int_or0((pre + "I").c_str()); r.s = int_or0((pre + "S").c_str()); r.cs = int_or0((pre + "CS").c_str());
            e.refs.push_back(r);
        }
        out.squad_list.entries.push_back(std::move(e));
    }
    for (const auto& cat : cam->references) if (cat.first == "zones") for (const md::fcs::Reference& z : cat.second) out.zones.emplace_back(z.values[0], z.values[1]);

    for (const md::fcs::Record& p : quick.records) {
        if (p.kind != K_PLATOON) continue;
        const std::string* fac = FindStr(p, "faction name");
        if (!fac || *fac != out.faction_name) continue;   // another faction's squad
        KSquad sq;
        const std::string* sid = FindStr(p, "platoon stringID");
        sq.id = sid ? *sid : p.name;
        if (const std::string* v = FindStr(p, "faction stringID")) sq.faction = *v;
        if (const auto* v = FindVec3(p, "position")) for (int i = 0; i < 3; ++i) sq.position[i] = (*v)[i];
        md::fcs::File pf;
        std::string content = "platoon/" + sq.id + ".platoon";   // the record names its file in "content file" (exe 0x140372ef0)
        for (const auto& kv : p.files) if (kv.first == "content file" && !kv.second.empty()) content = kv.second;
        if (!Load(fs::path(dir) / content, pf, err)) return false;
        std::map<std::string, const md::fcs::Record*> by_id;
        for (const md::fcs::Record& r : pf.records) by_id[r.string_id] = &r;
        for (const md::fcs::Record& coll : pf.records) {
            if (coll.kind != K_INSTANCES) continue;
            for (const md::fcs::Instance& in : coll.instances) {
                KChar c; c.template_id = in.target;
                for (int i = 0; i < 3; ++i) c.position[i] = in.position[i];
                for (int i = 0; i < 4; ++i) c.rotation[i] = in.rotation[i];
                for (const std::string& st : in.states) {
                    auto it = by_id.find(st); if (it == by_id.end()) continue;
                    const md::fcs::Record& s = *it->second;
                    if (s.kind == K_CHARACTER_STATE) { if (const std::string* n = FindStr(s, "name")) c.name = *n; }
                    else if (s.kind == K_STATS) c.stats = s.floats;
                    else if (s.kind == K_MEDICAL) {
                        c.dead = FindBool(s, "dead"); c.unconscious = FindBool(s, "unconcious"); c.incapacitated = FindBool(s, "incapacitated"); c.coma = FindBool(s, "coma");
                        if (const float* v = FindFloat(s, "KO")) c.ko = *v;
                        if (const float* v = FindFloat(s, "bleeding")) c.bleeding = *v;
                        if (const float* v = FindFloat(s, "blood")) c.blood = *v;
                        if (const float* v = FindFloat(s, "hung")) c.hunger = *v;
                        if (const float* v = FindFloat(s, "fed")) c.fed = *v;
                        for (int n = 0;; ++n) {   // 0x14064d600
                            const std::string k = std::to_string(n);
                            const char* const names[7] = {"hit", "hitmult", "flesh", "stun", "bandage", "rig", "wear"};
                            KPart pt; bool any = false;
                            if (const std::string* sid = FindStr(s, ("sid" + k).c_str())) { pt.sid = *sid; any = true; }
                            float* fld[7] = {&pt.hit, &pt.hitmult, &pt.flesh, &pt.stun, &pt.bandage, &pt.rig, &pt.wear};
                            for (int q = 0; q < 7; ++q) if (const float* v = FindFloat(s, (std::string(names[q]) + k).c_str())) { *fld[q] = *v; any = true; }
                            if (!any) break;
                            c.parts.push_back(pt);
                        }
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
    md::fcs::Error e;
    md::fcs::File quick; quick.header.filetype = md::fcs::FILETYPE_V15;
    md::fcs::Record cam = Rec(K_CAMERA, 1, "camera", "1-quick-INGAME");
    int members = 0; for (const KSquad& sq : s.squads) members += (int)sq.characters.size();
    cam.ints = { {"time day", s.day}, {"time hour", s.hour}, {"time minute", s.minute}, {"player money", s.money}, {"squads", (int)s.squads.size()}, {"members", members} };   // "squads" / "members": counts the exe writes (0x140375360)
    if (s.orders.present) {   // 0x1403eb710 visits the keys in this order: ditch sleep feed heal help rescue share stay sit eject shootfirst, then the int formation
        static const char* const ORDER[] = {"ditch", "sleep", "feed", "heal", "help", "rescue", "share", "stay", "sit", "eject", "shootfirst"};
        for (const char* k : ORDER) for (const OrderBool& b : ORDER_BOOLS) if (!strcmp(b.key, k)) cam.bools.push_back({k, s.orders.*b.field});
        cam.ints.push_back({"formation", s.orders.formation});
    }
    if (s.sky.present) {   // 0x14066d7d0: floats current time, total time, cloud density, cloud speed; bool updating; vec3 ambient mult, its speed
        const KSky& k = s.sky;
        cam.floats.insert(cam.floats.end(), { {"sky update current time", k.current_time}, {"sky update total time", k.total_time}, {"sky clouds density", k.clouds_density}, {"sky clouds density speed", k.clouds_speed} });
        cam.bools.push_back({"sky updating", k.updating});
    }
    for (size_t i = 0; i < s.squad_list.entries.size(); ++i) {   // 0x1402deea0: per entry the string, the float, then the references (TYPE C I S CS, 0x1406bca80)
        const KSquadListEntry& e = s.squad_list.entries[i]; const std::string n = std::to_string(i);
        cam.strings.push_back({"sq" + n, e.name}); cam.floats.push_back({"sqtime" + n, e.time});
        for (size_t j = 0; j < e.refs.size(); ++j) {
            const std::string pre = n + ":sq list" + std::to_string(j); const KSquadRef& r = e.refs[j];
            cam.ints.push_back({pre + "TYPE", r.type}); cam.ints.push_back({pre + "C", r.c}); cam.ints.push_back({pre + "I", r.i}); cam.ints.push_back({pre + "S", r.s}); cam.ints.push_back({pre + "CS", r.cs});
        }
    }
    if (s.squad_list.has_prosperity) cam.floats.push_back({"prosperity", s.squad_list.prosperity});
    cam.vec3s = { {"pos", {s.camera[0], s.camera[1], s.camera[2]}} };
    cam.strings.insert(cam.strings.begin(), { {"pfaction name", s.faction_name}, {"version", s.version} });
    std::vector<md::fcs::Reference> zr; for (const auto& z : s.zones) { md::fcs::Reference r; r.values[0] = z.first; r.values[1] = z.second; r.values[2] = 1; zr.push_back(r); }   // exe 0x140375360: (x, z, 1) per active zone
    if (s.sky.present) {
        cam.vec3s.push_back({"sky ambient color mult", {s.sky.ambient[0], s.sky.ambient[1], s.sky.ambient[2]}});
        cam.vec3s.push_back({"sky ambient color mult speed", {s.sky.ambient_speed[0], s.sky.ambient_speed[1], s.sky.ambient_speed[2]}});
    }
    cam.references = { {"zones", zr} };
    quick.records.push_back(cam);
    int next = 2;
    for (const KSquad& sq : s.squads) {
        md::fcs::Record p = Rec(K_PLATOON, next, sq.id, std::to_string(next) + "-quick-INGAME"); ++next;
        p.strings = { {"platoon stringID", sq.id}, {"faction name", s.faction_name}, {"faction stringID", sq.faction} };
        p.vec3s = { {"position", {sq.position[0], sq.position[1], sq.position[2]}} };
        p.bools = { {"never been activated", false}, {"intact", true} };   // exe 0x140372ef0: a loaded squad is written with these and its file name
        p.ints = { {"char count", (int)sq.characters.size()}, {"squad index", 0} };
        p.files = { {"content file", "platoon/" + sq.id + ".platoon"} };
        quick.records.push_back(p);
        md::fcs::File pf; pf.header.filetype = md::fcs::FILETYPE_V15;
        md::fcs::Record coll = Rec(K_INSTANCES, 1, "characters", "1-" + sq.id + "-INGAME");
        int id = 2;
        for (const KChar& c : sq.characters) {
            md::fcs::Instance in; in.id = std::to_string(id) + "-" + sq.id; in.target = c.template_id;
            for (int i = 0; i < 3; ++i) in.position[i] = c.position[i];
            for (int i = 0; i < 4; ++i) in.rotation[i] = c.rotation[i];
            auto add = [&](md::fcs::Record r) { in.states.push_back(r.string_id); pf.records.push_back(std::move(r)); };
            auto mk = [&](int kind) { const int i = id++; return Rec(kind, i, c.name, std::to_string(i) + "-" + sq.id); };
            md::fcs::Record g = mk(K_CHARACTER_STATE); g.strings = { {"name", c.name} }; add(g);
            md::fcs::Record st = mk(K_STATS); st.floats = c.stats; add(st);
            md::fcs::Record m = mk(K_MEDICAL);
            m.bools = { {"incapacitated", c.incapacitated}, {"unconcious", c.unconscious}, {"dead", c.dead}, {"coma", c.coma} };   // exe 0x14064ea50
            m.floats = { {"blood", c.blood}, {"KO", c.ko}, {"hung", c.hunger}, {"fed", c.fed}, {"bleeding", c.bleeding} };
            for (size_t n = 0; n < c.parts.size(); ++n) {   // 0x14064d390: sid<n>, then hit hitmult flesh stun bandage rig wear
                const KPart& pt = c.parts[n]; const std::string k = std::to_string(n);
                m.strings.push_back({"sid" + k, pt.sid});
                m.floats.push_back({"hit" + k, pt.hit}); m.floats.push_back({"hitmult" + k, pt.hitmult}); m.floats.push_back({"flesh" + k, pt.flesh}); m.floats.push_back({"stun" + k, pt.stun});
                m.floats.push_back({"bandage" + k, pt.bandage}); m.floats.push_back({"rig" + k, pt.rig}); m.floats.push_back({"wear" + k, pt.wear});
            }
            add(m);
            coll.instances.push_back(std::move(in));
        }
        pf.records.insert(pf.records.begin(), coll);
        pf.header.id_counter = id;
        if (!md::fcs::WriteFile((fs::path(dir) / "platoon" / (sq.id + ".platoon")).string(), pf, e)) { err = e.Message(); return false; }
    }
    quick.header.id_counter = next;
    if (!md::fcs::WriteFile((fs::path(dir) / "quick.save").string(), quick, e)) { err = e.Message(); return false; }
    return true;
}

}  // namespace md::save

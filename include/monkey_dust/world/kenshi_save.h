// Kenshi save folder (quick.save + platoon/<squad>.platoon, all FCS v15): the player's squads, their characters and the world clock.
// Load-time code (std::string / std::vector on purpose). Field names and record kinds follow the FCS itemType enum and Project Okran's
// kenshi_save.rs (MIT, hint); the exe's own reader (SaveManager, ksdp) is not yet read against them, see docs/research/SAVE_Q09_ENTRYPOINTS_2026-11-19.md.
#pragma once
#include <string>
#include <utility>
#include <vector>

namespace md::save {

// One body part of the medical record (exe writer 0x14064d390, loader 0x14064d600): `sid<n>` string, then seven floats keyed by name + part index n.
// The exe reads one part per part of the character's body; this reader takes parts n = 0.. while `sid<n>` or one of the seven float keys exists.
struct KPart { std::string sid; float hit = 0.f, hitmult = 0.f, flesh = 0.f, stun = 0.f, bandage = 0.f, rig = 0.f, wear = 0.f; };

struct KChar {
    std::string template_id, name;                  // the CHARACTER record placed, GAMESTATE_CHARACTER "name"
    float position[3] = {0, 0, 0};
    float rotation[4] = {0, 0, 0, 0};               // as stored: w, x, y, z
    std::vector<std::pair<std::string, float>> stats;   // STATS floats in file order
    bool dead = false, unconscious = false, incapacitated = false, coma = false;   // MEDICAL_STATE bools (exe 0x14064ea50; "unconcious" is the exe's spelling)
    float blood = 100.f, hunger = 2.f, fed = 0.f, ko = 0.f, bleeding = 0.f;       // MEDICAL_STATE floats "blood" / "hung" / "fed" / "KO" / "bleeding"
    std::vector<KPart> parts;
};

// The stat names of a STATS record in the order the exe writes them (0x14064a8f0; 44 keys: the last is "free attribute points").
const std::vector<std::string>& StatKeys();
// A stat as the exe's loader (0x14064b6a0) reads it from the record's float map: a missing key reads 0, an unknown key is ignored, "toughness2" absent = the "defence" value, "lockpicking" absent = "thievery" * 0.5.
float StatValue(const KChar& c, const std::string& key);

struct KSquad {
    std::string id, faction;
    float position[3] = {0, 0, 0};
    std::vector<KChar> characters;
};

// Player squad order toggles and formation (exe writer 0x1403eb710, loader 0x1403ebf50). `present` is false when the record has no "ditch" key:
// the exe loader then leaves its own defaults alone, so a save without the keys keeps these defaults too.
struct KOrders {
    bool present = false;
    bool heal = false, help = false, rescue = false, stay = false, feed = false, share = false, sleep = false, ditch = false, sit = false, eject = false, shootfirst = false;
    int formation = 0;
};

// Sky state of the world clock (exe writer 0x14066d7d0, loader 0x14066db50). Defaults are what the exe loader sets when the float `sky update current time` is missing
// (`present` false): updating on, speeds 0, ambient multiplier (1,1,1), cloud density 0.25. A saved `sky updating` = false is turned back on by the loader and
// current / total time and both speeds are reset to 0 (so a loaded KSky is never `updating == false`).
struct KSky {
    bool present = false;
    bool updating = true;
    float current_time = 0.f, total_time = 0.f;
    float ambient[3] = {1.f, 1.f, 1.f}, ambient_speed[3] = {0.f, 0.f, 0.f};
    float clouds_density = 0.25f, clouds_speed = 0.f;
};

// Squad list of the state record (exe writer 0x1402deea0, loader 0x1402df0c0): entry i is the string `sq<i>` (a data record name), the float `sqtime<i>` and a list of
// references stored as ints under `<i>:sq list<j>` + TYPE / C / I / S / CS (0x1406bca80 writes, 0x1406bd210 + 0x1406bd000 read). `prosperity` (0x1402e0b90 / 0x1402e08d0) is a float
// the exe keeps as a double. DEVIATION: the exe loader skips an entry whose name is not found in the game data and applies the rest onto its existing objects; this reader keeps every entry.
struct KSquadRef { int type = 0, c = 0, i = 0, s = 0, cs = 0; };
struct KSquadListEntry { std::string name; float time = 0.f; std::vector<KSquadRef> refs; };
struct KSquadList { bool has_prosperity = false; float prosperity = 0.f; std::vector<KSquadListEntry> entries; };

// Selection (exe writer 0x1407f3e60, loader 0x1407f3fb0) and the last shown biome text (0x140728af0 / 0x140728c10) of the state record. A reference that is not stored reads
// as the exe's null reference (type 11, rest 0, DAT_141e3a600). `selected_characters` keys are the name + j + suffix, the two single ones the name + suffix.
struct KSelection {
    bool present = false;
    std::vector<KSquadRef> characters;
    KSquadRef character{11, 0, 0, 0, 0}, gui_object{11, 0, 0, 0, 0};
};
struct KBiomeText { bool present = false; float timer = 0.f; std::string name; };

struct KenshiSave {
    int day = 1, hour = 12, minute = 0;
    int money = 0;
    float camera[3] = {0, 0, 0};
    std::string faction_name, version;
    std::vector<std::pair<int, int>> zones;
    KOrders orders;
    KSky sky;
    KSquadList squad_list;
    KSelection selection;
    KBiomeText biome_text;
    std::vector<KSquad> squads;                     // the player's faction only
};

// Reads `dir`/quick.save and the player's platoon files. False with a readable message in `err` on a missing, truncated or otherwise
// damaged file (the message names the file and the byte offset); a squad's missing platoon file is an error too.
bool ReadKenshiSave(const std::string& dir, KenshiSave& out, std::string& err);

// Writes the same set as v15 files (`dir` and `dir`/platoon are created). For tests and for our own saves in Kenshi's layout.
bool WriteKenshiSave(const std::string& dir, const KenshiSave& s, std::string& err);

}  // namespace md::save

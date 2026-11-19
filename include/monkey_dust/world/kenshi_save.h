// Kenshi save folder (quick.save + platoon/<squad>.platoon, all FCS v15): the player's squads, their characters and the world clock.
// Load-time code (std::string / std::vector on purpose). Field names and record kinds follow the FCS itemType enum and Project Okran's
// kenshi_save.rs (MIT, hint); the exe's own reader (SaveManager, ksdp) is not yet read against them, see docs/research/SAVE_Q09_ENTRYPOINTS_2026-11-19.md.
#pragma once
#include <string>
#include <utility>
#include <vector>

namespace md::save {

struct KChar {
    std::string template_id, name;                  // the CHARACTER record placed, GAMESTATE_CHARACTER "name"
    float position[3] = {0, 0, 0};
    float rotation[4] = {0, 0, 0, 0};               // as stored: w, x, y, z
    std::vector<std::pair<std::string, float>> stats;   // STATS floats in file order
    bool dead = false, unconscious = false;         // MEDICAL_STATE "dead" / "unconcious" (sic)
    float blood = 100.f, hunger = 2.f, fed = 0.f;   // MEDICAL_STATE "blood" / "hung" / "fed"
};

struct KSquad {
    std::string id, faction;
    float position[3] = {0, 0, 0};
    std::vector<KChar> characters;
};

struct KenshiSave {
    int day = 1, hour = 12, minute = 0;
    int money = 0;
    float camera[3] = {0, 0, 0};
    std::string faction_name, version;
    std::vector<std::pair<int, int>> zones;
    std::vector<KSquad> squads;                     // the player's faction only
};

// Reads `dir`/quick.save and the player's platoon files. False with a readable message in `err` on a missing, truncated or otherwise
// damaged file (the message names the file and the byte offset); a squad's missing platoon file is an error too.
bool ReadKenshiSave(const std::string& dir, KenshiSave& out, std::string& err);

// Writes the same set as v15 files (`dir` and `dir`/platoon are created). For tests and for our own saves in Kenshi's layout.
bool WriteKenshiSave(const std::string& dir, const KenshiSave& s, std::string& err);

}  // namespace md::save

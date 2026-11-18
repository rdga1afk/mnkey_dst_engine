#pragma once
// The squad of a NEW_GAME_STARTOFF record (record kind 64) as the exe's new-game start (0x140872e00, KSDP notes) lists it: every `squad` reference names a CHARACTER record and carries
// the member count in its first value (0 gives 1). The positions are a separate step: the exe puts each member at the start position plus a uniform random offset in -50..50 on x and z
// (constants 0x141683170 / 0x141700da8 = +50 / -50), the draws are the caller's.
#include <monkey_dust/io/fcs_gamedata.h>
#include <string>
#include <vector>

namespace md::world {

constexpr int32_t kNewGameStartoffKind = 64;
constexpr float kStartJitterMetres = 50.f;

struct StartMember {
    std::string id;     // CHARACTER record string id
    std::string name;   // its record name
};

inline std::vector<StartMember> StartSquadMembers(const md::fcs::GameDataContainer& data, const std::string& start_record_name) {
    std::vector<StartMember> out;
    for (const md::fcs::GameData* rec : data.OfType(kNewGameStartoffKind)) {
        if (rec->name != start_record_name) continue;
        const auto* refs = rec->Refs("squad");
        if (!refs) break;
        for (const auto& r : *refs) {
            const int n = r.second[0] > 0 ? r.second[0] : 1;
            const md::fcs::GameData* ch = data.Get(r.first);
            for (int k = 0; k < n; ++k) out.push_back({r.first, ch ? ch->name : std::string()});
        }
        break;
    }
    return out;
}

} // namespace md::world

#pragma once
// Who a spawned NPC looks like, from the data, in the order the exe's character assembly uses (CPU only, load time).
//  race : squad template `race override`, else the CHARACTER's `race`, else the `races` list of the squad's faction (weights = first int of each entry).
//         A character that is an ANIMAL_CHARACTER skips the template override. A faction that has no `races` gets the default race `17-gamedata.quack`. No faction either: `resolved` stays false (no stand-in body).
//  sex  : a race with `single gender` is male; else female when a random float is below `female chance` * 0.01.
//  head : one of the race's `heads male` / `heads female` HEAD records that have a `texture map` file, weighted by the entry's first int.
//  hair : one of the race's `hairs` ATTACHMENT records with `attach slot` 2; beard (males only): slot 13. Weight = entry's first int clamped to 1..100.
//  hair colour: one entry of the race's `hair colors` (COLOR_DATA), weighted by the entry's first int; its `color 1` int is the ARGB colour.
// DEVIATION: the exe's random helper is not read; draws come from a splitmix64 stream seeded by `seed`. The weights of `hair colors` are taken as the entry's first int (the exe lookup is not read).
// UNKNOWN: skin tone and the other slider ranges (not read), the faction of a squad that has no `faction` reference.
#include <monkey_dust/io/fcs_gamedata.h>
#include <monkey_dust/world/town_spawn.h>
#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace md::world {

constexpr int32_t kRaceKind = 7, kAnimalCharacterKind = 76;

struct NpcLook {
    bool resolved = false;
    std::string race, head, hair, beard, hair_colour;   // record string ids ("" = none)
    bool female = false;
    uint32_t hair_argb = 0;
    std::string mesh, body_texture;                      // install paths as the race record holds them
};

namespace detail {
inline float Unit(uint64_t& s) { return (float)(Mix(s) >> 40) / 16777216.0f; }
// Weighted pick over refs in list order (cumulative, normalised); `keep(id)` filters, `weight(entry)` gives the weight. Returns the index or -1.
template <class Keep, class Weight>
int PickWeighted(const std::vector<std::pair<std::string, std::array<int32_t, 3>>>& refs, uint64_t& rng, Keep keep, Weight weight) {
    float total = 0; for (const auto& r : refs) if (keep(r.first)) total += weight(r);
    int first = -1, count = 0; for (size_t i = 0; i < refs.size(); ++i) if (keep(refs[i].first)) { if (first < 0) first = (int)i; ++count; }
    if (first < 0) return -1;
    if (count == 1) return first;
    if (total <= 0) { int n = Rand(rng, 0, count - 1); for (size_t i = 0; i < refs.size(); ++i) if (keep(refs[i].first) && n-- == 0) return (int)i; }
    const float u = Unit(rng) * total; float acc = 0; int last = first;
    for (size_t i = 0; i < refs.size(); ++i) { if (!keep(refs[i].first)) continue; acc += weight(refs[i]); last = (int)i; if (u < acc) return last; }
    return last;
}
}  // namespace detail

// `faction_id`: the squad's faction record (the template's `faction`, else the town owner); may be empty.
inline NpcLook ResolveNpcLook(const md::fcs::GameDataContainer& data, const std::string& character_id, const std::string& template_id, const std::string& faction_id, uint32_t seed) {
    NpcLook out; uint64_t rng = ((uint64_t)seed << 32) ^ detail::Fnv(character_id) ^ (detail::Fnv(template_id) << 1);
    const md::fcs::GameData* ch = data.Get(character_id); if (!ch) return out;
    const md::fcs::GameData* tpl = data.Get(template_id);
    auto hasAny = [](const std::vector<std::pair<std::string, std::array<int32_t, 3>>>* r) { return r && !r->empty(); };
    auto raceWeight = [](const std::pair<std::string, std::array<int32_t, 3>>& e) { return (float)e.second[0]; };
    auto any = [](const std::string&) { return true; };
    const std::vector<std::pair<std::string, std::array<int32_t, 3>>>* src = nullptr;
    if (tpl && ch->kind != kAnimalCharacterKind && hasAny(tpl->Refs("race override"))) src = tpl->Refs("race override");
    if (!src && hasAny(ch->Refs("race"))) src = ch->Refs("race");
    if (!src) { const md::fcs::GameData* f = faction_id.empty() ? nullptr : data.Get(faction_id); if (f && hasAny(f->Refs("races"))) src = f->Refs("races"); }
    std::vector<std::pair<std::string, std::array<int32_t, 3>>> fallback;
    if (!src && !faction_id.empty() && data.Get(faction_id)) { fallback.push_back({ "17-gamedata.quack", { 100, 0, 0 } }); src = &fallback; }   // a faction without `races` gets the one default race
    if (!src) return out;
    const int ri = detail::PickWeighted(*src, rng, any, raceWeight); if (ri < 0) return out;
    const md::fcs::GameData* race = data.Get((*src)[(size_t)ri].first); if (!race || race->kind != kRaceKind) return out;
    out.race = race->string_id;
    out.female = !detail::Flag(race, "single gender") && detail::Unit(rng) < (float)detail::IntOf(ch, "female chance") * 0.01f;
    auto file = [&](const char* k) { auto it = race->files.find(k); return it == race->files.end() ? std::string() : it->second; };
    out.mesh = file(out.female ? "female mesh" : "male mesh");
    out.body_texture = file(out.female ? "body texture female" : "body texture male");
    if (const auto* heads = race->Refs(out.female ? "heads female" : "heads male")) {
        auto hasTex = [&](const std::string& id) { const auto* h = data.Get(id); if (!h) return false; auto it = h->files.find("texture map"); return it != h->files.end() && !it->second.empty(); };
        const int hi = detail::PickWeighted(*heads, rng, hasTex, raceWeight); if (hi >= 0) out.head = (*heads)[(size_t)hi].first;
    }
    if (const auto* hairs = race->Refs("hairs")) {
        auto slotOf = [&](const std::string& id) { const auto* a = data.Get(id); return a ? detail::IntOf(a, "attach slot") : -1; };
        auto clamp = [](const std::pair<std::string, std::array<int32_t, 3>>& e) { const int w = e.second[0]; return (float)(w < 1 ? 1 : (w > 100 ? 100 : w)); };
        const int hi = detail::PickWeighted(*hairs, rng, [&](const std::string& id) { return slotOf(id) == 2; }, clamp); if (hi >= 0) out.hair = (*hairs)[(size_t)hi].first;
        if (!out.female) { const int bi = detail::PickWeighted(*hairs, rng, [&](const std::string& id) { return slotOf(id) == 13; }, clamp); if (bi >= 0) out.beard = (*hairs)[(size_t)bi].first; }
    }
    if (const auto* cols = race->Refs("hair colors")) {
        const int ci = detail::PickWeighted(*cols, rng, any, raceWeight);
        if (ci >= 0) { out.hair_colour = (*cols)[(size_t)ci].first; if (const auto* c = data.Get(out.hair_colour)) out.hair_argb = (uint32_t)detail::IntOf(c, "color 1"); }
    }
    out.resolved = true;
    return out;
}

}  // namespace md::world

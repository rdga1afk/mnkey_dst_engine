#pragma once
// Who a town's squads are made of, from the data, as the exe builds them (CPU only, load time; the switch that wires it to the NPC spawn path is MD_TOWN_NPC).
//  - Owner: the TOWN record's `faction` reference (first entry), else the faction record `nofac` («No Faction»): RootObjectFactory::process, TOWN branch.
//  - Resident templates: the town's own `residents` plus the owner's `residents` unless the town's bool `residents override` is set (the town resident reader).
//  - Members of a template (RootObjectFactory::createRandomSquad): the categories leader, squad, squad2, animals, animals2 in that order, every entry a CHARACTER record with
//    min (value 0) and max (value 1); max above 0 and not 100 gives a random count in min..max, else min. `num random chars` / `num random chars max` add draws from `choosefrom list`
//    (one random entry per draw). States set by the exe: leader 2, squad 0, squad2 1, animals 0, animals2 1, choosefrom 0 (animals 2 then reset to 0 in the exe).
// DEVIATION: the exe's random helper is not read; the draws here come from a splitmix64 stream seeded by `seed` and the template id, so the same seed gives the same list.
// UNKNOWN (not read): the exe's day / night rule (all residents are listed at once), `slaves` / `prisoners` recursion, the squad's home building and the spawn point offsets.
#include <monkey_dust/io/fcs_gamedata.h>
#include <cstdint>
#include <type_traits>
#include <string>
#include <vector>

namespace md::world {

constexpr int32_t kTownKind = 13, kSquadTemplateKind = 52;
constexpr const char* kNoFactionId = "nofac";

struct TownSpawnEntry {
    std::string character;    // CHARACTER record string id
    std::string name;         // its record name
    std::string template_id;  // the SQUAD_TEMPLATE it came from
    std::string category;     // leader / squad / squad2 / animals / animals2 / choosefrom list
    int state = 0;
    bool named = false;       // the character record has `unique` or `named`: listed apart, the seed does not decide it
};

struct TownSpawnList {
    bool found = false;
    std::string owner;                       // string id of the owner faction record ("nofac" when the town has no `faction`)
    std::vector<std::string> templates;      // resident templates in order (town first, then owner)
    std::vector<TownSpawnEntry> anonymous, named;
};

namespace detail {
inline uint64_t Mix(uint64_t& s) { s += 0x9e3779b97f4a7c15ull; uint64_t z = s; z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull; z = (z ^ (z >> 27)) * 0x94d049bb133111ebull; return z ^ (z >> 31); }
inline int Rand(uint64_t& s, int lo, int hi) { if (hi <= lo) return lo; return lo + (int)(Mix(s) % (uint64_t)(hi - lo + 1)); }
inline uint64_t Fnv(const std::string& t) { uint64_t h = 1469598103934665603ull; for (unsigned char c : t) { h ^= c; h *= 1099511628211ull; } return h; }
inline bool Flag(const md::fcs::GameData* g, const char* k) { if (!g) return false; auto it = g->bools.find(k); return it != g->bools.end() && it->second; }
inline int IntOf(const md::fcs::GameData* g, const char* k) { if (!g) return 0; auto it = g->ints.find(k); return it == g->ints.end() ? 0 : it->second; }
}  // namespace detail

inline std::string TownOwner(const md::fcs::GameData& town) {
    const auto* refs = town.Refs("faction");
    return (refs && !refs->empty()) ? refs->front().first : std::string(kNoFactionId);
}

inline TownSpawnList MakeTownSpawnList(const md::fcs::GameDataContainer& data, const std::string& town_id, uint32_t seed) {
    TownSpawnList out; const md::fcs::GameData* town = data.Get(town_id);
    if (!town || town->kind != kTownKind) return out;
    out.found = true; out.owner = TownOwner(*town);
    auto add = [&](const md::fcs::GameData* g) { if (!g) return; if (const auto* r = g->Refs("residents")) for (const auto& e : *r) out.templates.push_back(e.first); };
    add(town);
    if (!detail::Flag(town, "residents override")) add(data.Get(out.owner));
    static const struct { const char* cat; int state; } kCats[] = { {"leader", 2}, {"squad", 0}, {"squad2", 1}, {"animals", 0}, {"animals2", 1} };
    for (const std::string& tid : out.templates) {
        const md::fcs::GameData* t = data.Get(tid); if (!t) continue;
        uint64_t rng = ((uint64_t)seed << 32) ^ detail::Fnv(tid);
        auto push = [&](const std::string& ch, const char* cat, int state) {
            const md::fcs::GameData* c = data.Get(ch);
            TownSpawnEntry e{ ch, c ? c->name : std::string(), tid, cat, state, detail::Flag(c, "unique") || detail::Flag(c, "named") };
            (e.named ? out.named : out.anonymous).push_back(e);
        };
        for (const auto& c : kCats) {
            const auto* refs = t->Refs(c.cat); if (!refs) continue;
            for (const auto& r : *refs) {
                const int mn = r.second[0], mx = r.second[1];
                const int n = (mx > 0 && mx != 100) ? detail::Rand(rng, mn, mx) : mn;
                for (int k = 0; k < n; ++k) push(r.first, c.cat, c.state);
            }
        }
        const auto* pool = t->Refs("choosefrom list");
        if (pool && !pool->empty()) {
            const int lo = detail::IntOf(t, "num random chars"), hi = detail::IntOf(t, "num random chars max");
            const int n = hi > 0 ? detail::Rand(rng, lo, hi) : lo;
            for (int k = 0; k < n; ++k) push((*pool)[(size_t)detail::Rand(rng, 0, (int)pool->size() - 1)].first, "choosefrom list", 0);
        }
    }
    return out;
}

// The NPC budget cap and the placement seam. `placer(entry, index, x, z)` gives a member's position and returns false when it has none; the position RULE of the exe (home building, spawn point) is not read,
// so the game supplies no placer yet and only the tests do. Members are taken in list order (anonymous first, then named) until `budget` are placed. No allocation beyond the returned vector.
constexpr size_t kTownNpcBudget = 64;
struct PlacedMember { const TownSpawnEntry* entry = nullptr; float x = 0.f, z = 0.f, y = 0.f; bool has_y = false; };   // y only when the placer supplies it (a node's own height)
template <class Placer>
std::vector<PlacedMember> PlanTownSpawn(const TownSpawnList& list, size_t budget, Placer&& placer) {
    std::vector<PlacedMember> out; out.reserve(budget);
    size_t i = 0;
    auto take = [&](const std::vector<TownSpawnEntry>& v) { for (const auto& e : v) { if (out.size() >= budget) return; PlacedMember m; m.entry = &e; bool ok; if constexpr (std::is_invocable_v<Placer&, const TownSpawnEntry&, size_t, float&, float&, float&>) { ok = placer(e, i++, m.x, m.z, m.y); m.has_y = ok; } else ok = placer(e, i++, m.x, m.z); if (ok) out.push_back(m); } };
    take(list.anonymous); take(list.named);
    return out;
}

}  // namespace md::world

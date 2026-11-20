#pragma once
// The squad spawn nodes of the buildings around a point, in world metres (DE-364). The exe places a town's squad at a random LocationNode of node type 1 («general node»), else type 0 («shopkeeper node»);
// nodes are the `is node` records (`node type`) placed inside a building's interior, which `data/newland/leveldata/interiors.level` stores per building type in building-local coordinates
// (FCS units, 0.1 m). World position = the placed building's transform (the zone's `0-buildinglist`, as zone_buildings.h) applied to the local position.
// Exe read 20.11: the town OF A POINT (an NPC, a spawn) is the nearest town whose radius holds it (a 3x3 zone-cell search), the radius being TownRadiusM below (the exe radius virtual; the Hub: type 2, mult 1 -> 2500 m).
// That is not the building list: the town handle's own building list is not read. DEVIATION: the buildings of a town are taken by position, every building placed within `size radius` (350 m for the Hub), (docs/DEVIATIONS.md).
// UNKNOWN: the offset the exe adds to the node position, bar squads.
#include <monkey_dust/io/fcs_gamedata.h>
#include <monkey_dust/world/town_spawn.h>
#include <cstdint>
#include <string>
#include <vector>

namespace md::world {

// The exe's town radius in metres (read ok; the branch of a town without an owner object): by the town's `type` 2 -> 2500, 3 -> 937.5, 4 -> 875, 9 -> 437.5, 8 and 10 -> 0, others 1250,
// times `town radius mult`, which the town init clamps to 0.1..5 (a value below 0.1 becomes 1; docs/research/TOWN_RESIDENTS_TRACE_2026-11-19.md).
inline float TownRadiusM(int type, float mult) {
    const float m = mult < 0.1f ? 1.f : (mult > 5.f ? 5.f : mult);
    switch (type) { case 2: return m * 2500.f; case 3: return m * 937.5f; case 4: return m * 875.f; case 9: return m * 437.5f; case 8: case 10: return 0.f; default: return m * 1250.f; }
}

// A town of the townlist, in world metres, with what the exe radius reads (`type`, `town radius mult`).
struct TownSite { std::string id; float x = 0.f, z = 0.f; int type = 0; float mult = 1.f; };
// Every TOWN record placed by `0-townlist` of Newwworld then rebirth (a later folder wins a town, as LoadTowns). Load time only.
bool LoadTownSites(const std::string& kenshi_dir, const md::fcs::GameDataContainer& data, std::vector<TownSite>& out, std::string& err);

// The exe's town of a point: the nearest town whose radius (TownRadiusM, 0 = none) holds the point; -1 when none does.
inline int TownOfPoint(const std::vector<TownSite>& towns, float x, float z) {
    int best = -1; float bd = 3.4028235e38f;
    for (size_t i = 0; i < towns.size(); ++i) {
        const float r = TownRadiusM(towns[i].type, towns[i].mult); if (r <= 0.f) continue;
        const float dx = towns[i].x - x, dz = towns[i].z - z, d2 = dx * dx + dz * dz;
        if (d2 <= r * r && d2 < bd) { best = (int)i; bd = d2; }
    }
    return best;
}

struct TownNode {
    std::string building;   // BUILDING record id of the placed building
    int node_type = 0;      // `node type` of the node record: 0 shopkeeper, 1 general, 2 guard post (not a member node)
    int placed = -1;        // index of the placed building in the town's building list (the housing unit)
    float x = 0.f, y = 0.f, z = 0.f;   // metres in our world
};

// Nodes of type 0, 1 and 2 of every building the town holds: a placed building belongs to the town TownOfPoint gives for its position (DE-364, replaces the circle of `size radius`). Load time only.
bool LoadTownNodes(const std::string& kenshi_dir, const md::fcs::GameDataContainer& data, const std::vector<TownSite>& towns, int town, std::vector<TownNode>& out, std::string& err);
// Number of placed buildings of the town that carry a node (the building count of the membership).
inline size_t TownBuildingCount(const std::vector<TownNode>& nodes) { int hi = -1; for (const auto& n : nodes) if (n.placed > hi) hi = n.placed; return (size_t)(hi + 1); }

// The exe's choice (the squad spawner): one random node of type 1, and only when the town has none of type 1 one of type 0; index drawn uniformly (DEVIATION: the exe's random helper is not read, the caller
// supplies the draw `r` in 0..2^32-1). Returns nullptr when the town has no such node (the exe then keeps the position it was given).
inline const TownNode* ChooseSpawnNode(const std::vector<TownNode>& nodes, uint32_t r) {
    size_t n1 = 0, n0 = 0; for (const auto& n : nodes) { if (n.node_type == 1) ++n1; else if (n.node_type == 0) ++n0; }
    const int want = n1 ? 1 : 0; const size_t cnt = n1 ? n1 : n0; if (!cnt) return nullptr;
    size_t k = (size_t)(((uint64_t)r * cnt) >> 32);
    for (const auto& n : nodes) if (n.node_type == want) { if (k == 0) return &n; --k; }
    return nullptr;
}

// The squad's own spawn point: a node of the squad's housed building (`placed`), the first node type with a node in the order 0, 2, 1; one of them by the draw `r`. nullptr = the origin.
inline const TownNode* SquadSpawnNode(const std::vector<TownNode>& nodes, int placed, uint32_t r) {
    for (int type : { 0, 2, 1 }) {
        size_t cnt = 0; for (const auto& n : nodes) if (n.placed == placed && n.node_type == type) ++cnt;
        if (!cnt) continue;
        size_t k = (size_t)(((uint64_t)r * cnt) >> 32);
        for (const auto& n : nodes) if (n.placed == placed && n.node_type == type) { if (k == 0) return &n; --k; }
    }
    return nullptr;
}

// Housing: the wanted-building table is walked in six passes, priority 5 down to 0; an entry with count left takes the next squad without a house and its count drops by one (DE-364).
// UNKNOWN: where the exe gets each entry's priority and count (the registration is not read), and the pick weights of the exe (interior 1.0, exterior 0.4, preferred x10, avoided x0.05) are not applied:
// the table is the caller's input and an entry names its building by `placed`. Returns for each squad the `placed` index it got, -1 for a squad left without.
struct WantedBuilding { int placed = -1; int priority = 0; int count = 0; };
inline std::vector<int> AssignHousing(std::vector<WantedBuilding> table, size_t squads) {
    std::vector<int> home(squads, -1); size_t next = 0;
    for (int p = 5; p >= 0; --p)
        for (auto& e : table) {
            if (e.priority != p) continue;
            while (e.count > 0 && next < squads) { home[next++] = e.placed; --e.count; }
        }
    return home;
}

// The placer for PlanTownSpawn: every member goes to ChooseSpawnNode with a draw from a splitmix64 stream of `seed`, so the same seed gives the same positions. A member with no node is not placed.
struct NodePlacer {
    const std::vector<TownNode>& nodes; uint64_t state;
    NodePlacer(const std::vector<TownNode>& n, uint32_t seed) : nodes(n), state(0x5851f42d4c957f2dull ^ seed) {}
    bool operator()(const TownSpawnEntry&, size_t, float& x, float& z, float& y) {   // y = the node's own world height (building transform applied to the local node)
        const TownNode* n = ChooseSpawnNode(nodes, (uint32_t)(detail::Mix(state) >> 32)); if (!n) return false;
        x = n->x; z = n->z; y = n->y; return true;
    }
};

}  // namespace md::world

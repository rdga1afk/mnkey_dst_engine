#pragma once
// The squad spawn nodes of the buildings around a point, in world metres (DE-364). The exe places a town's squad at a random LocationNode of node type 1 («general node»), else type 0 («shopkeeper node»);
// nodes are the `is node` records (`node type`) placed inside a building's interior, which `data/newland/leveldata/interiors.level` stores per building type in building-local coordinates
// (FCS units, 0.1 m). World position = the placed building's transform (the zone's `0-buildinglist`, as zone_buildings.h) applied to the local position.
// UNKNOWN: which buildings the exe counts as a town's (here: every building placed within the circle), the offset the exe adds to the node position, bar squads.
#include <monkey_dust/io/fcs_gamedata.h>
#include <cstdint>
#include <string>
#include <vector>

namespace md::world {

struct TownNode {
    std::string building;   // BUILDING record id of the placed building
    int node_type = 0;      // `node type` of the node record: 0 shopkeeper, 1 general
    float x = 0.f, y = 0.f, z = 0.f;   // metres in our world
};

// Nodes of type 0 and 1 of every building whose placement lies in the circle (centre and radius in metres). `interiors_level` is the path of interiors.level. Load time only.
bool LoadTownNodes(const std::string& kenshi_dir, const md::fcs::GameDataContainer& data, float cx_m, float cz_m, float radius_m, std::vector<TownNode>& out, std::string& err);

// The exe's choice (the squad spawner): one random node of type 1, and only when the town has none of type 1 one of type 0; index drawn uniformly (DEVIATION: the exe's random helper is not read, the caller
// supplies the draw `r` in 0..2^32-1). Returns nullptr when the town has no such node (the exe then keeps the position it was given).
inline const TownNode* ChooseSpawnNode(const std::vector<TownNode>& nodes, uint32_t r) {
    size_t n1 = 0, n0 = 0; for (const auto& n : nodes) (n.node_type == 1 ? n1 : n0)++;
    const int want = n1 ? 1 : 0; const size_t cnt = n1 ? n1 : n0; if (!cnt) return nullptr;
    size_t k = (size_t)(((uint64_t)r * cnt) >> 32);
    for (const auto& n : nodes) if (n.node_type == want) { if (k == 0) return &n; --k; }
    return nullptr;
}

}  // namespace md::world

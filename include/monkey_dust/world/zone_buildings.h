// Placed building parts of Kenshi zone files (`data/newland/leveldata/<folder>/zone.X.Y.zone`, FCS v15). Load time only.
//
// Placement chain (checked on the Hub, #119): the zone's `0-buildinglist` record holds one instance per placed
// building (position, rotation, target = BUILDING id, first state = the zone record carrying `world Y pos`);
// BUILDING -> `scale` + references `parts`; BUILDING_PART -> file `phs or mesh`. Part offsets are not applied
// (as in the spike input tests/data/hub_spike/). Parts whose mesh name contains "black" are interior masks and are skipped.
// Folder order: rebirth wins over Newwworld (Project Okran ZONE_FOLDERS, MIT, src/world/town.rs; unverified assumption).
#pragma once
#include <monkey_dust/io/fcs_gamedata.h>
#include <string>
#include <vector>

namespace md::world {

constexpr float ZONE_UNITS = 4608.0f;        // one zone side, 0.1 m units (460.8 m)
constexpr float WORLD_ORIGIN_UNITS = 147456.0f;

struct PlacedPart {
    std::string mesh;       // as stored, '\\' -> '/'
    float x, y, z;          // metres in our world; y = `world Y pos` / 10
    float y_off;            // instance position y / 10: the offset above the terrain, metres
    float scale;
    float quat[4];          // w x y z
};

// Zone files that touch the circle (centre and radius in metres), one per zone, preferred folder first.
std::vector<std::string> ZoneFilesInCircle(const std::string& kenshi_dir, float cx_m, float cz_m, float radius_m);

// Every part of every building inside the circle (centre cx,cz and radius in metres).
bool LoadBuildingsInCircle(const std::string& kenshi_dir, const md::fcs::GameDataContainer& data,
                           float cx_m, float cz_m, float radius_m, std::vector<PlacedPart>& out, std::string& err);

// Same, loading the merged game data of the install itself (load order of the install's mods.cfg).
bool LoadBuildingsAt(const std::string& kenshi_dir, float cx_m, float cz_m, float radius_m, std::vector<PlacedPart>& out, std::string& err);

} // namespace md::world

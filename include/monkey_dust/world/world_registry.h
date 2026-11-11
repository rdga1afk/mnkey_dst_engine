#pragma once
#include <monkey_dust/world/biome_def.h>
// WorldRegistry — persistent per-zone player state + active zone tracking.
// Zones built from the install (kind-95 weather groups + areasmap regions); save/load game/data/world_registry.txt.

#include <cstring>
#include <cstdlib>
#include <cstdint>

static constexpr int WR_MAX_ZONES = 128;
static constexpr int WR_NAME_LEN  = 48;

struct ZoneRecord {
    char  name[WR_NAME_LEN]         = {};  // slug id (e.g. "great_desert")
    char  display_name[WR_NAME_LEN] = {};  // human-readable (e.g. "Great Desert")
    char  biome[24]                 = {};  // desert/volcanic/swamp/canyon/scrubland/highlands
    char  factions_str[64]          = {};  // comma-separated faction slugs
    float exit_x      = 0.f;
    float exit_z      = 0.f;
    float amplitude   = 40.f;
    float fog_density = 0.3f;
    float fog_dist_m  = 800.f;
    int   grid_x      = 0;
    int   grid_z      = 0;
    int   danger      = 1;
    bool  visited     = false;
};

class WorldRegistry {
public:
    static WorldRegistry& Get() { static WorldRegistry s; return s; }

    // Load/save per-zone player state (visited, exit pos, amplitude overrides)
    void Load(const char* path = "game/data/world_registry.txt");
    void Save(const char* path = "game/data/world_registry.txt") const;

    const char* CurrentZone()   const { return current_zone_; }
    float       PlayerX()       const { return player_x_; }
    float       PlayerZ()       const { return player_z_; }

    void SetCurrentZone(const char* name) {
        strncpy(current_zone_, name, WR_NAME_LEN - 1);
    }
    void SetPlayerPos(float x, float z) { player_x_ = x; player_z_ = z; }

    ZoneRecord* Find(const char* name) {
        for (int i = 0; i < zone_count_; ++i)
            if (!strcmp(zones_[i].name, name)) return &zones_[i];
        return nullptr;
    }

    // Lookup by zone grid coords — used to keep current_zone_ (and hence
    // the world map's exact player-position marker, WorldMapUI::Draw's
    // state==2 branch) in sync with real per-tick player movement, not
    // just explicit fast-travel/portal transitions (see FindByGrid's
    // call sites: main.cpp's HandleTerrainStreaming, the same real
    // zone-boundary-crossing check the 9x9 streaming window already uses).
    ZoneRecord* FindByGrid(int gx, int gz) {
        for (int i = 0; i < zone_count_; ++i)
            if (zones_[i].grid_x == gx && zones_[i].grid_z == gz) return &zones_[i];
        return nullptr;
    }

    ZoneRecord* GetAll(int& count) { count = zone_count_; return zones_; }
    int         Count() const      { return zone_count_; }

    // Add or get zone record
    ZoneRecord* GetOrCreate(const char* name) {
        ZoneRecord* r = Find(name);
        if (r) return r;
        if (zone_count_ >= WR_MAX_ZONES) return nullptr;
        ZoneRecord& nr = zones_[zone_count_++];
        nr = ZoneRecord{};
        strncpy(nr.name, name, WR_NAME_LEN - 1);
        return &nr;
    }

    // The zone list from the install (owner 11.11): one zone per FCS kind-95 weather group (name, areasmap colour `index`); its cell is a cell inside the group's areasmap region (the
    // region pixel nearest to the region's centroid, 256 x 256 texels -> 64 x 64 cells), so the terrain window origin (zone_ox/oz = cell - TNKN/2) is the real position. Zones without a
    // region on the map are skipped. Returns the number of zones (0 = no install data: the registry stays empty).
    int  BuildZonesFromInstall();
    // The biome of every zone cell: the BIOMES record of the install that owns the colour of the cell centre in biomemap.png (exact legend colour, BiomeRegistry::FindByLegend).
    bool BuildBiomeMapFromInstall();   // false without an install

    // Get biome string for any zone by grid coords (returns "desert" if unknown)
    const char* GetBiomeAt(int gx, int gz) const {
        if (gx < 0 || gx > 63 || gz < 0 || gz > 63) return "desert";
        return BiomeRegistry::Get().SlugAt(biome_grid_[gz][gx]);   // the slug of the FCS BIOMES record that owns the cell colour
    }

private:
    uint8_t    biome_grid_[64][64] = {};  // per zone cell: the BiomeRegistry load-order index of the BIOMES record that owns the cell colour

    WorldRegistry() { BuildZonesFromInstall(); BuildBiomeMapFromInstall(); Load(); }
    ZoneRecord zones_[WR_MAX_ZONES];
    int        zone_count_  = 0;
    char       current_zone_[WR_NAME_LEN] = "border_zone";
    float      player_x_    = 0.f;
    float      player_z_    = 0.f;
};

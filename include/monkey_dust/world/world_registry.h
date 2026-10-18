#pragma once
// WorldRegistry — persistent per-zone player state + active zone tracking.
// Zones loaded from game/data/terrain_config.txt; save/load game/data/world_registry.txt.

#include <cstring>
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

    // Load terrain config (zone definitions) — called from constructor
    void LoadFromTerrainConfig(const char* path = "game/data/terrain_config.txt");

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

    // Load game/data/biome_map.txt (64×64 grid of zx,zz=biome lines)
    void LoadBiomeMap(const char* path = "game/data/biome_map.txt");


    // Get biome string for any zone by grid coords (returns "desert" if unknown)
    const char* GetBiomeAt(int gx, int gz) const {
        if (gx < 0 || gx > 63 || gz < 0 || gz > 63) return "desert";
        return kBiomeNames[biome_grid_[gz][gx]];
    }

private:
    static constexpr const char* kBiomeNames[] = {
        "desert","highland","scrubland","swamp","canyon","volcanic","coast","forest","ashlands","jungle","highlands"
    };
    uint8_t    biome_grid_[64][64] = {};  // biome index per zone cell

    WorldRegistry() { LoadFromTerrainConfig(); LoadBiomeMap(); Load(); }
    ZoneRecord zones_[WR_MAX_ZONES];
    int        zone_count_  = 0;
    char       current_zone_[WR_NAME_LEN] = "border_zone";
    float      player_x_    = 0.f;
    float      player_z_    = 0.f;
};

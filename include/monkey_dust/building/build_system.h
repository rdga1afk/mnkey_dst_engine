#pragma once
#include <monkey_dust/building/production_chain.h>
#include <monkey_dust/ecs/md_registry.h>
#include <monkey_dust/components/building.h>
#include <monkey_dust/components/inventory.h>
#include <monkey_dust/world/world_transform.h>
#include <cstdint>
#include <cmath>

static constexpr int MAX_BUILDING_DEFS = 512;
static constexpr int MAX_BUILDINGS     = 8192;
static constexpr int MAX_GRID          = 200;
static constexpr int GRID_HALF         = MAX_GRID / 2;

struct BuildingDef {
    uint32_t        id;
    char            name[32];
    int             size_x, size_z;
    ItemStack       build_cost[PROD_MAX_SLOTS];
    int             build_cost_count;
    ProductionChain chain;
    bool            loaded;
    uint8_t         _pad[3];
    // Kenshi RE: min_dismantle_materials_percentage (+0x4d, config×0.01)
    float           dismantle_pct = 0.4f;  // fraction of materials returned on demolish
    // B-1: Kenshi RE: fuel/resource consumption per game-hour (CLAUDE_KEN_MECH_BUILDINGS §4)
    // fuel_item_id=0 means no fuel required. Consumed from building's linked storage.
    uint32_t        fuel_item_id      = 0;   // ItemDef id of required fuel (0=none)
    float           fuel_per_hour     = 0.f; // units of fuel consumed per game-hour (1Hz×3600)
    uint32_t        power_item_id     = 0;   // ItemDef id of power source (0=grid/passive)
    float           power_consumption = 0.f; // kW draw (used by PowerGrid)
};

class BuildSystem {
public:
    static BuildSystem& Get() { static BuildSystem inst; return inst; }

    int  LoadFromFile(const char* path);
    MdEntity Place(float wx, float wz, uint32_t def_id, Inventory& player_inv);
    void Demolish(MdEntity e, Inventory& player_inv);
    void Tick(float dt_s);

    static void WorldToGrid(float wx, float wz, int& gx, int& gz) {
        gx = (int)(wx) + GRID_HALF;
        gz = (int)(wz) + GRID_HALF;
    }
    static void GridToWorld(int gx, int gz, float& wx, float& wz) {
        wx = (float)(gx - GRID_HALF) + 0.5f;
        wz = (float)(gz - GRID_HALF) + 0.5f;
    }
    static void SnapToGrid(float& wx, float& wz) {
        wx = floorf(wx) + 0.5f;
        wz = floorf(wz) + 0.5f;
    }

    const BuildingDef* GetDef(uint32_t id) const;
    int DefCount() const { return def_count_; }
    const BuildingDef* GetDefByIndex(int idx) const {
        if (idx < 0 || idx >= def_count_) return nullptr;
        return &defs_[idx];
    }
    bool IsCellOccupied(int gx, int gz) const {
        if (gx < 0 || gx >= MAX_GRID || gz < 0 || gz >= MAX_GRID) return true;
        return grid_[gx][gz] != MdEntity::Null();
    }
    void RebuildGridFromEntities();

private:
    BuildSystem();
    BuildingDef  defs_[MAX_BUILDING_DEFS];
    int          def_count_ = 0;
    MdEntity grid_[MAX_GRID][MAX_GRID];
};

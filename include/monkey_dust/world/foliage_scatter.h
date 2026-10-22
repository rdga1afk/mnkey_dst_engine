// Foliage meshes scattered over a zone as Kenshi's FoliageSystem does when a zone loads (F041803 layer scatter, F041802 one object, F041815 / F041775 random numbers).
// Load time only (std::vector / std::string on purpose); world units are Kenshi's (0.1 m).
//
// Behaviour ported from Project Okran (MIT, Copyright (c) 2026 brayniac6-glitch): src/world/foliage.rs (Foliage::load, Mt, scatter_layer, place) and src/world/town.rs
// (nearest). See THIRD_PARTY.md. The two generator functions were checked against the decompiled exe (Ghidra, 1406d87f0 and 1406cb960): tempering, the double product
// and the mask-and-retry loop are the ones below.
#pragma once
#include <monkey_dust/io/fcs_gamedata.h>
#include <array>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace md::world {

// Kenshi's global MT19937 (0x142131e90).
class Mt {
public:
    explicit Mt(uint32_t seed);
    uint32_t NextU32();
    float Range(float a, float b);   // F041815: a + (b - a) x tempered / 2^32, in double, then float
    uint32_t Int(uint32_t n);        // F041775: an integer in [0, n]: outputs masked to n's bits until one is no larger
private:
    void Reload();
    uint32_t state_[624];
    int index_ = 624;
};

struct FoliageMesh {   // a FOLIAGE_MESH (F041792)
    std::string record, mesh, texture, leaves_mesh, leaves_texture;   // files of the record: "mesh", "texture map", "leaves mesh", "leaves texture"
    float max_slope = 0, min_slope = 0, min_altitude = 0, max_altitude = 0;
    bool floating = false, clustered = false;
    float cluster_num_min = 0, cluster_num_max = 0, cluster_radius_min = 0, cluster_radius_max = 0;
    bool limit_to_grass = false;
    float child_cluster_radius = 0, road_avoidance = 0, grass_spot = 0;
    bool avoid_towns = false, building_type = false;
    float min_height = 0, max_height = 0, vertical_offset_min = 0, vertical_offset_max = 0;
    int material_type = 0;   // MapFeatureMode: 2 = TERRAIN (textured with the biome's terrain layers; its own texture fields are ignored), 4 = FOLIAGE
    std::vector<int> children;
};

struct FoliageLayer {   // a FOLIAGE_LAYER that has meshes
    std::string record;
    int32_t seed_base = 0;   // parseInt of the string id
    std::vector<std::pair<int, float>> meshes;   // mesh index, count per zone
    int visibility_range = 0;   // 0 CLOSE, 1 MEDIUM, 2 FAR
    bool wind = false;
};

struct Grass {   // a GRASS record as the overlay painting takes it (F041791)
    std::string record;
    float density = 0, min_altitude = 0, max_altitude = 0, max_slope = 0;
    float noise_scale = 1, zero_cutoff = 0, blackout_noise_scale = 1, blackout_zero_cutoff = 0, cap = 1, brightness_boost = 1;
    bool blackout = false;
};

struct BiomeCliff { std::string texture; float tiling_x = 1.f, tiling_y = 1.f; };   // the biome's cliff layer ("texture vertical", "tiling X/Y 2"): one repeat per 5000 units / tiling

struct Foliage {
    std::vector<FoliageMesh> meshes;
    std::vector<FoliageLayer> layers;
    std::unordered_map<std::string, std::vector<int>> biome_layers;   // BIOMES string id -> its layers with meshes, in reference order
    std::vector<Grass> grass;
    std::unordered_map<std::string, std::vector<int>> biome_grass;    // BIOMES string id -> the grass of all its layers, in reference order
    std::unordered_map<int, BiomeCliff> biome_cliff;                  // by BIOMES "index"
    std::unordered_map<int, std::string> biome_by_index;              // BIOMES "index" (the biomemap colour, r << 16 | g << 8 | b) -> string id
};

struct Placed { int mesh, layer; float x, y, z, yaw, scale; int biome = -1; };   // yaw in degrees about y; biome = the BIOMES index of the zone biome whose layer placed it (ScatterZone), -1 otherwise

struct Town { float x, z, no_foliage; int kind; };

// The towns the avoid-towns rule reads: the TOWN records placed by the instances of kind 38 in `0-townlist`... of each leveldata.level (Newwworld, then rebirth; a later folder wins a town), Okran town.rs.
bool LoadTowns(const std::string& kenshi_dir, const md::fcs::GameDataContainer& data, std::vector<Town>& out, std::string& err);

// What the scatter reads of the world (world units).
class FoliageGround {
public:
    virtual ~FoliageGround() = default;
    virtual float Height(float x, float z) = 0;
    virtual float Slope(float x, float z) = 0;
    virtual int BiomeIndex(float x, float z) = 0;
    virtual bool Overlay(float x, float z, uint8_t px[4]) = 0;   // the overlay pixel as Ogre has it in memory (B, G, R, A); false off the map
    virtual bool OverlayCell(float x, float z, int* col, int* row, float* cell) = 0;
    virtual bool OverlayAt(int col, int row, uint8_t px[4]) = 0;
    virtual bool OverlayCentre(int col, int row, float* x, float* z) = 0;   // an inner pixel's centre; false on the rim or off the map
    virtual void SetOverlayGreen(int col, int row, uint8_t v) = 0;
    virtual float WaterLevel() = 0;
};

constexpr int kNoBiome = -1;   // scatter without the per-place biome check

int32_t ParseIntOgre(const std::string& s);   // Ogre StringConverter::parseInt: the leading integer, 0 when none
Foliage LoadFoliage(const md::fcs::GameDataContainer& data);

// One layer's meshes scattered over the zone rect {x0, z0, x1, z1}; biome = the biome index the places must be in (kNoBiome: the zone has one biome).
void ScatterLayer(const Foliage& f, int layer, const float rect[4], int biome, const std::vector<Town>& towns, FoliageGround& ground, std::vector<Placed>& out);

// A zone's whole scatter of one visibility range (0 CLOSE meshes, 1 MEDIUM, 2 FAR): its ground (heights of fullmap.tif, the overlay window, the biomemap), the grass painted into the
// overlay first when a layer needs it, then every layer of every biome present. Zone indices 0..63; positions in Kenshi units with the origin shifted by -147456 (as the zone files).
// `towns` are the avoided towns (type 8 excluded by the caller or by NearestTown). False + err when a file of the install is missing.
bool ScatterZone(const std::string& kenshi_dir, const Foliage& f, int zx, int zz, int visibility, const std::vector<Town>& towns, std::vector<Placed>& out, std::string& err, const std::vector<int>* only_biomes = nullptr);   // only_biomes: restrict to these BIOMES indices (the oracle of the python stand-in knows one biome)

} // namespace md::world

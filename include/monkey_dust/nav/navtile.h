// Kenshi's navmesh tiles (data/newland/land/navtiles/tileX.Y.hkt) as sections + a Detour navmesh built from a section (#A5 prep).
// Facts: a tile file holds the tile's ground navmesh and one navmesh per building in it ("NavMesh" / "Graph" / "Info" entries of the root
// container's namedVariants). Havok metres = world units x 0.1, Y up. The ground mesh is relative to the tile corner (tile X.Y covers zone cell
// X.Y, ZONE_SIZE 4608 units = 460.8 m, world 64 x 64 tiles, centre at 14745.6 m); a building mesh is relative to the building position its
// "Info" string gives ("uid hex;x;y;z", world units), unrotated. Face data: one int per face (faceDataStriding), Kenshi's face type
// (3 water, 4 costly, 5 blocked, -1 / 1 plain ground).
#pragma once
#include <cstdint>
#include <string>
#include <vector>

class dtNavMesh;

namespace md::navtile {

struct Face { uint32_t start_edge = 0, num_edges = 0; int32_t cluster = 0, kind = -1; };
struct Edge { uint32_t a = 0, b = 0; int32_t opposite_face = -1, opposite_edge = -1; };
struct Section {
    int tile_x = 0, tile_y = 0;
    bool building = false;
    std::string info;
    std::vector<float> vertices;   // xyz triples, metres, world placed (x, z from the world corner at 0)
    std::vector<Face> faces;
    std::vector<Edge> edges;
};

constexpr int kTiles = 64;
constexpr float kZoneMetres = 460.8f;
constexpr float kWorldMetres = 29491.2f;

// Tile (x, y) containing a world position in metres measured from the world corner (the engine's world coordinates).
inline int TileOf(float metres) { return (int)(metres / kZoneMetres); }

// Every section of tile (x, y), positions in engine world metres (corner origin); empty and true when the tile has no file.
bool LoadTile(const std::string& kenshi_dir, int x, int y, std::vector<Section>& out, std::string& err);

// Detour navmesh of one ground section: faces triangulated in XZ (ear clipping), neighbours from the shared directed vertex pairs,
// area 1 = ground, 2 = costly (face type 4), blocked faces (type 5) and water (3) are left out. Returns null + err on failure.
dtNavMesh* BuildDetour(const Section& s, std::string& err);

struct PolyStats { long faces = 0, kept = 0, split = 0, dropped = 0, triangles = 0, polys = 0, portal_edges = 0; };

// The Detour tile data (dtCreateNavMeshData blob, free with dtFree) of one ground section of navtile (tx, ty); *data_out is null (true returned) when it has no walkable polygon. *npolys = its polygon count.
bool BuildTileData(const Section& s, int tx, int ty, unsigned char** data_out, int* size, int* npolys, PolyStats* stats, std::string& err);

// One Detour navmesh with a tile per ground section of the tiles x0..x1, y0..y1 (inclusive, navtile grid): convex polygons of <= 6 vertices (a convex face stays one polygon, the others are ear clipped and merged),
// border edges on the tile lines get Detour portal codes so neighbouring tiles link. cs 0.05 m, ch 0.02 m, the Detour tile grid is the 460.8 m navtile grid with its origin at the world corner.
bool BuildDetourTiles(const std::string& kenshi_dir, int x0, int y0, int x1, int y1, dtNavMesh** out, PolyStats* stats, std::string& err);

}  // namespace md::navtile

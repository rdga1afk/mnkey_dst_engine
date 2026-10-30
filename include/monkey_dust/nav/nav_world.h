// The ground navmesh of the Kenshi world: Detour tiles built from data/newland/land/navtiles/tile<x>.<y>.hkt (src/nav/navtile.cpp) for the 3x3 tiles
// around the camera, built on a worker thread (the tile data is cached in ~/.cache/monkey_dust/navtiles-v1), added / removed on the calling thread
// (the worker adds a built tile to the navmesh under nav_mu_, Tick only does the bookkeeping and the removal). Positions are engine world metres (origin at the world corner, the same as md.terrain_height).
#pragma once
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

class dtNavMesh;
class dtNavMeshQuery;

namespace md::nav {

class NavWorld {
public:
    static NavWorld& Get();
    struct Stats { int tiles_live = 0, tiles_requested = 0; long loaded_total = 0, unloaded_total = 0, cache_hits = 0; double last_add_ms = 0, max_add_ms = 0, last_worker_ms = 0, max_worker_ms = 0, max_tick_ms = 0; };   // add_ms = the worker's addTile under the lock; tick_ms = the calling thread's Tick

    void SetEnabled(bool on, const std::string& kenshi_dir);   // starts / stops the worker thread and drops every tile
    bool Enabled() const { return enabled_; }
    void Tick(float cam_x, float cam_z);                       // request the 3x3 tiles around the camera, add at most one built tile, drop the unwanted ones
    // Straight path of up to max_pts points (x, y, z triples into out) from (ax, az) to (bx, bz); false when a position has no polygon within 20 m in the live tiles. *complete is false when the goal is not reached.
    bool FindPath(float ax, float az, float bx, float bz, float* out, int max_pts, int* n_pts, bool* complete);
    const Stats& GetStats() const { return stats_; }

private:
    struct Built { int x, y; uint64_t ref; bool empty; bool from_cache; double ms, add_ms; };
    struct Live { int x, y; uint64_t ref; double last_wanted_s; bool empty; };
    void WorkerMain();
    void Shutdown();
    bool enabled_ = false, stop_ = false;
    std::string dir_;
    std::thread worker_;
    std::mutex mu_;
    std::mutex nav_mu_;   // the navmesh: the worker adds tiles, FindPath queries, Tick removes (try_lock)
    std::condition_variable cv_;
    std::deque<std::pair<int, int>> requests_;
    std::vector<Built> results_;
    std::vector<Live> live_;
    std::vector<std::pair<int, int>> pending_;   // requested, not yet answered
    dtNavMesh* nav_ = nullptr;
    dtNavMeshQuery* query_ = nullptr;
    Stats stats_;
};

}  // namespace md::nav

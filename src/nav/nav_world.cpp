#include <monkey_dust/nav/nav_world.h>
#include <monkey_dust/nav/navtile.h>
#include <monkey_dust/platform/md_log.h>
#include <DetourAlloc.h>
#include <DetourNavMesh.h>
#include <DetourNavMeshQuery.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>

namespace md::nav {
namespace {
constexpr int kMaxTiles = 64;
constexpr double kUnloadS = 20.0;
constexpr uint32_t kCacheMagic = 0x3154564eu;   // "NVT1"
double NowS() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
std::string CacheDir() {
    const char* xdg = std::getenv("XDG_CACHE_HOME"); const char* home = std::getenv("HOME");
    std::string base = xdg && *xdg ? xdg : (home ? std::string(home) + "/.cache" : std::string("."));
    return base + "/monkey_dust/navtiles-v1";
}
struct CacheHead { uint32_t magic; int32_t size; uint64_t src_size; int64_t src_mtime; };
}  // namespace

NavWorld& NavWorld::Get() { static NavWorld w; return w; }

void NavWorld::SetEnabled(bool on, const std::string& kenshi_dir) {
    if (on == enabled_) return;
    if (!on) { Shutdown(); return; }
    dir_ = kenshi_dir;
    nav_ = dtAllocNavMesh();
    dtNavMeshParams p; std::memset(&p, 0, sizeof p);
    p.tileWidth = navtile::kZoneMetres; p.tileHeight = navtile::kZoneMetres; p.maxTiles = kMaxTiles; p.maxPolys = 1 << 16;
    if (!nav_ || dtStatusFailed(nav_->init(&p))) { MD_LOG(MD_LOG_WARNING, "[NavWorld] navmesh init failed"); dtFreeNavMesh(nav_); nav_ = nullptr; return; }
    query_ = dtAllocNavMeshQuery(); query_->init(nav_, 4096);
    stop_ = false; stats_ = Stats{};
    worker_ = std::thread([this] { WorkerMain(); });
    enabled_ = true;
}

void NavWorld::Shutdown() {
    if (worker_.joinable()) { { std::lock_guard<std::mutex> lk(mu_); stop_ = true; } cv_.notify_all(); worker_.join(); }
    for (Built& b : results_) dtFree(b.data);
    results_.clear(); requests_.clear(); pending_.clear(); live_.clear();
    if (query_) { dtFreeNavMeshQuery(query_); query_ = nullptr; }
    if (nav_) { dtFreeNavMesh(nav_); nav_ = nullptr; }
    enabled_ = false; stats_ = Stats{};
}

void NavWorld::WorkerMain() {
    for (;;) {
        std::pair<int, int> t;
        {
            std::unique_lock<std::mutex> lk(mu_);
            cv_.wait(lk, [this] { return stop_ || !requests_.empty(); });
            if (stop_) return;
            t = requests_.front(); requests_.pop_front();
        }
        const auto t0 = std::chrono::steady_clock::now();
        Built b{t.first, t.second, nullptr, 0, false, 0.0};
        char name[96]; std::snprintf(name, sizeof name, "/data/newland/land/navtiles/tile%d.%d.hkt", t.first, t.second);
        const std::string src = dir_ + name; char cname[64]; std::snprintf(cname, sizeof cname, "/tile%d.%d.bin", t.first, t.second);
        const std::string cache = CacheDir() + cname;
        std::error_code ec; const uint64_t ssz = std::filesystem::exists(src, ec) ? (uint64_t)std::filesystem::file_size(src, ec) : 0;
        const int64_t smt = ssz ? (int64_t)std::filesystem::last_write_time(src, ec).time_since_epoch().count() : 0;
        if (ssz) {
            std::ifstream in(cache, std::ios::binary); CacheHead h{};
            if (in && in.read((char*)&h, sizeof h) && h.magic == kCacheMagic && h.src_size == ssz && h.src_mtime == smt && h.size >= 0) {
                if (h.size == 0) { b.from_cache = true; }
                else if (unsigned char* d = (unsigned char*)dtAlloc((size_t)h.size, DT_ALLOC_PERM)) {
                    if (in.read((char*)d, h.size)) { b.data = d; b.size = h.size; b.from_cache = true; } else dtFree(d);
                }
            }
            if (!b.from_cache) {
                std::vector<navtile::Section> secs; std::string err;
                if (!navtile::LoadTile(dir_, t.first, t.second, secs, err)) MD_LOG(MD_LOG_WARNING, "[NavWorld] tile %d.%d: %s", t.first, t.second, err.c_str());
                else {
                    for (const navtile::Section& s : secs) {
                        if (s.building) continue;
                        int np = 0;
                        if (!navtile::BuildTileData(s, t.first, t.second, &b.data, &b.size, &np, nullptr, err)) { MD_LOG(MD_LOG_WARNING, "[NavWorld] tile %d.%d build: %s", t.first, t.second, err.c_str()); b.data = nullptr; b.size = 0; }
                        break;   // the ground section is the first
                    }
                    std::filesystem::create_directories(CacheDir(), ec);
                    std::ofstream out(cache, std::ios::binary | std::ios::trunc); CacheHead hh{kCacheMagic, b.size, ssz, smt};
                    out.write((const char*)&hh, sizeof hh); if (b.size) out.write((const char*)b.data, b.size);
                }
            }
        }
        b.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        std::lock_guard<std::mutex> lk(mu_);
        results_.push_back(b);
    }
}

void NavWorld::Tick(float cam_x, float cam_z) {
    if (!enabled_) return;
    const double now = NowS();
    const int cx = navtile::TileOf(cam_x), cz = navtile::TileOf(cam_z);
    for (int dz = -1; dz <= 1; ++dz) for (int dx = -1; dx <= 1; ++dx) {
        const int x = cx + dx, z = cz + dz;
        if (x < 0 || z < 0 || x >= navtile::kTiles || z >= navtile::kTiles) continue;
        bool known = false;
        for (Live& l : live_) if (l.x == x && l.y == z) { l.last_wanted_s = now; known = true; break; }
        if (known) continue;
        for (auto& p : pending_) if (p.first == x && p.second == z) { known = true; break; }
        if (known) continue;
        pending_.emplace_back(x, z);
        { std::lock_guard<std::mutex> lk(mu_); requests_.emplace_back(x, z); }
        cv_.notify_one();
    }
    Built b{}; bool have = false;
    { std::lock_guard<std::mutex> lk(mu_); if (!results_.empty()) { b = results_.front(); results_.erase(results_.begin()); have = true; } }
    if (have) {
        pending_.erase(std::remove(pending_.begin(), pending_.end(), std::make_pair(b.x, b.y)), pending_.end());
        stats_.last_worker_ms = b.ms; if (b.from_cache) ++stats_.cache_hits;
        const auto t0 = std::chrono::steady_clock::now();
        Live l{b.x, b.y, 0, now, b.data == nullptr};
        if (b.data) {
            dtTileRef ref = 0;
            if (dtStatusFailed(nav_->addTile(b.data, b.size, DT_TILE_FREE_DATA, 0, &ref))) { dtFree(b.data); l.empty = true; MD_LOG(MD_LOG_WARNING, "[NavWorld] addTile %d.%d failed", b.x, b.y); }
            else l.ref = ref;
        }
        live_.push_back(l); ++stats_.loaded_total;
        stats_.last_add_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        stats_.max_add_ms = std::max(stats_.max_add_ms, stats_.last_add_ms);
    }
    for (size_t i = 0; i < live_.size(); ++i) {
        if (now - live_[i].last_wanted_s <= kUnloadS) continue;
        if (live_[i].ref) { unsigned char* d = nullptr; int sz = 0; nav_->removeTile(live_[i].ref, &d, &sz); if (d) dtFree(d); }
        live_.erase(live_.begin() + (long)i); ++stats_.unloaded_total; break;
    }
    int n = 0; for (const Live& l : live_) if (l.ref) ++n;
    stats_.tiles_live = n; stats_.tiles_requested = (int)pending_.size();
}

bool NavWorld::FindPath(float ax, float az, float bx, float bz, float* out, int max_pts, int* n_pts, bool* complete) {
    *n_pts = 0; if (complete) *complete = false;
    if (!enabled_ || !query_ || max_pts < 2) return false;
    dtQueryFilter f; f.setIncludeFlags(1); f.setExcludeFlags(0);
    const float a[3] = {ax, 0.f, az}, b[3] = {bx, 0.f, bz}, ext[3] = {20.f, 600.f, 20.f};
    dtPolyRef ra = 0, rb = 0; float pa[3], pb[3];
    query_->findNearestPoly(a, ext, &f, &ra, pa); query_->findNearestPoly(b, ext, &f, &rb, pb);
    if (!ra || !rb) return false;
    static dtPolyRef path[2048]; int np = 0;
    query_->findPath(ra, rb, pa, pb, &f, path, &np, 2048);
    if (np <= 0) return false;
    static unsigned char fl[256]; static dtPolyRef pr[256]; static float sp[256 * 3];
    int ns = 0; query_->findStraightPath(pa, pb, path, np, sp, fl, pr, &ns, std::min(max_pts, 256));
    for (int i = 0; i < ns; ++i) { out[i * 3] = sp[i * 3]; out[i * 3 + 1] = sp[i * 3 + 1]; out[i * 3 + 2] = sp[i * 3 + 2]; }
    *n_pts = ns; if (complete) *complete = path[np - 1] == rb;
    return ns > 0;
}

}  // namespace md::nav

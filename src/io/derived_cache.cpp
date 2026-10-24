#include <monkey_dust/io/derived_cache.h>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <mutex>
#include <string>

namespace md {
namespace fs = std::filesystem;

namespace {
fs::path CacheFolder(uint64_t key) {
    const char* x = getenv("XDG_CACHE_HOME"); const char* h = getenv("HOME");
    fs::path base = (x && *x) ? fs::path(x) : (h && *h) ? fs::path(h) / ".cache" : fs::path();
    if (base.empty()) return {};
    char hex[20]; snprintf(hex, sizeof(hex), "%016llx", (unsigned long long)key);
    return base / "monkeydust" / hex;
}
std::mutex g_live_mu;
std::map<std::string, uint64_t>& LiveKeys() { static std::map<std::string, uint64_t> m; return m; }   // artifact name -> key requested in this process
void MarkLive(uint64_t key, const char* name) { std::lock_guard<std::mutex> g(g_live_mu); LiveKeys()[name] = key; }
}

uint64_t HashFiles(const std::vector<std::string>& files) {
    uint64_t h = 1469598103934665603ull;
    auto mix = [&](uint64_t v) { for (int i = 0; i < 8; ++i) { h ^= (v >> (8 * i)) & 0xff; h *= 1099511628211ull; } };
    std::error_code ec;
    for (const std::string& f : files) {
        mix((uint64_t)fs::file_size(f, ec));
        mix((uint64_t)fs::last_write_time(f, ec).time_since_epoch().count());
    }
    return h;
}

std::string CachePath(uint64_t key, const char* name) {
    MarkLive(key, name);
    const fs::path d = CacheFolder(key);
    return d.empty() ? std::string() : (d / name).string();
}

bool CacheRead(uint64_t key, const char* name, std::vector<uint8_t>& out) {
    MarkLive(key, name);
    const fs::path p = CacheFolder(key) / name;
    FILE* f = p.empty() ? nullptr : fopen(p.string().c_str(), "rb");
    fprintf(stderr, "[Cache] %s %016llx/%s\n", f ? "hit " : "miss", (unsigned long long)key, name);
    if (!f) return false;
    fseek(f, 0, SEEK_END); const long n = ftell(f); fseek(f, 0, SEEK_SET);
    out.resize(n > 0 ? (size_t)n : 0);
    const bool ok = n > 0 && fread(out.data(), 1, out.size(), f) == out.size();
    fclose(f);
    return ok;
}

bool CacheWrite(uint64_t key, const char* name, const uint8_t* data, size_t n) {
    MarkLive(key, name);
    const fs::path dir = CacheFolder(key);
    if (dir.empty()) return false;
    std::error_code ec; fs::create_directories(dir, ec);
    const fs::path tmp = dir / (std::string(name) + ".tmp"), dst = dir / name;
    FILE* f = fopen(tmp.string().c_str(), "wb");
    if (!f) return false;
    const bool ok = fwrite(data, 1, n, f) == n;
    fclose(f);
    fprintf(stderr, "[Cache] write %016llx/%s %zu bytes\n", (unsigned long long)key, name, n);
    if (!ok) { fs::remove(tmp, ec); return false; }
    fs::rename(tmp, dst, ec);
    return !ec;
}

int CacheCollectGarbage() {
    std::map<std::string, uint64_t> live; { std::lock_guard<std::mutex> g(g_live_mu); live = LiveKeys(); }
    if (live.empty()) return 0;
    const fs::path root = CacheFolder(0).parent_path();
    std::error_code ec; int removed = 0;
    if (root.empty() || !fs::is_directory(root, ec)) return 0;
    char cur[20];
    for (const fs::directory_entry& d : fs::directory_iterator(root, ec)) {
        if (!d.is_directory(ec)) continue;
        for (const fs::directory_entry& f : fs::directory_iterator(d.path(), ec)) {
            auto it = live.find(f.path().filename().string());
            if (it == live.end()) continue;   // an artifact this process did not ask for: not ours to judge
            snprintf(cur, sizeof(cur), "%016llx", (unsigned long long)it->second);
            if (d.path().filename() == cur) continue;
            fprintf(stderr, "[Cache] remove %s/%s (superseded by %s)\n", d.path().filename().string().c_str(), it->first.c_str(), cur);
            fs::remove(f.path(), ec); ++removed;
        }
        if (fs::is_empty(d.path(), ec)) fs::remove(d.path(), ec);
    }
    return removed;
}

}

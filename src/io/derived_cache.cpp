#include <monkey_dust/io/derived_cache.h>
#include <cstdio>
#include <cstdlib>
#include <filesystem>

namespace md {
namespace fs = std::filesystem;

namespace {
fs::path CacheFolder(uint64_t key) {
    const char* x = getenv("XDG_CACHE_HOME"); const char* h = getenv("HOME");
    fs::path base = (x && *x) ? fs::path(x) : (h && *h) ? fs::path(h) / ".cache" : fs::path();
    if (base.empty()) return {};
    char hex[20]; snprintf(hex, sizeof(hex), "%016llx", (unsigned long long)key);
    return base / "monkey_dust" / hex;
}
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
    const fs::path d = CacheFolder(key);
    return d.empty() ? std::string() : (d / name).string();
}

bool CacheRead(uint64_t key, const char* name, std::vector<uint8_t>& out) {
    const fs::path p = CacheFolder(key) / name;
    FILE* f = p.empty() ? nullptr : fopen(p.string().c_str(), "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END); const long n = ftell(f); fseek(f, 0, SEEK_SET);
    out.resize(n > 0 ? (size_t)n : 0);
    const bool ok = n > 0 && fread(out.data(), 1, out.size(), f) == out.size();
    fclose(f);
    return ok;
}

bool CacheWrite(uint64_t key, const char* name, const uint8_t* data, size_t n) {
    const fs::path dir = CacheFolder(key);
    if (dir.empty()) return false;
    std::error_code ec; fs::create_directories(dir, ec);
    const fs::path tmp = dir / (std::string(name) + ".tmp"), dst = dir / name;
    FILE* f = fopen(tmp.string().c_str(), "wb");
    if (!f) return false;
    const bool ok = fwrite(data, 1, n, f) == n;
    fclose(f);
    if (!ok) { fs::remove(tmp, ec); return false; }
    fs::rename(tmp, dst, ec);
    return !ec;
}

}

#include <monkey_dust/io/data_files.h>
#include <monkey_dust/io/derived_cache.h>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <vector>

namespace md {
namespace {
std::vector<EmbeddedFile>& Table() { static std::vector<EmbeddedFile> t; return t; }

uint64_t Fnv(uint64_t h, const void* p, size_t n) {
    const unsigned char* b = (const unsigned char*)p;
    for (size_t i = 0; i < n; ++i) { h ^= b[i]; h *= 1099511628211ull; }
    return h;
}

// 2048x2048, 12 mips, every block the same 8 / 16 bytes
std::string SolidDds(const char* fourcc, const unsigned char* block, size_t block_bytes) {
    uint32_t h[32] = {};
    const uint32_t W = 2048, H = 2048, MIPS = 12;
    size_t blocks = 0;
    for (uint32_t m = 0, w = W, hh = H; m < MIPS; ++m, w = w > 1 ? w / 2 : 1, hh = hh > 1 ? hh / 2 : 1)
        blocks += (size_t)((w + 3) / 4) * ((hh + 3) / 4);
    memcpy(&h[0], "DDS ", 4); h[1] = 124; h[2] = 0x000a1007; h[3] = H; h[4] = W;
    h[5] = (uint32_t)(((W + 3) / 4) * ((H + 3) / 4) * block_bytes); h[7] = MIPS;
    h[19] = 32; h[20] = 4; memcpy(&h[21], fourcc, 4); h[27] = 0x401008;
    std::string s((const char*)h, 128);
    s.reserve(128 + blocks * block_bytes);
    for (size_t i = 0; i < blocks; ++i) s.append((const char*)block, block_bytes);
    return s;
}
}

void RegisterEmbeddedFiles(const EmbeddedFile* files, size_t n) { for (size_t i = 0; i < n; ++i) Table().push_back(files[i]); }

const unsigned char* EmbeddedFileBytes(const char* path, size_t* size) {
    for (const EmbeddedFile& f : Table()) if (!strcmp(f.path, path)) { *size = f.size; return f.data; }
    return nullptr;
}

bool DevFilesEnabled() {
#ifdef MD_NO_DEV_PATHS
    return false;
#else
    const char* e = getenv("MD_DEV_FILES");
    return e && e[0] == '1';
#endif
}

std::string FlatNormalDds() { static const unsigned char b[8] = { 0x1F, 0x84, 0x1F, 0x84, 0, 0, 0, 0 }; return SolidDds("DXT1", b, 8); }
std::string MissingDiffuseDds() { static const unsigned char b[16] = { 0xff, 0xff, 0, 0, 0, 0, 0, 0, 0xff, 0xff, 0xff, 0xff, 0, 0, 0, 0 }; return SolidDds("DXT5", b, 16); }

std::string DevFile(const char* rel) {
    std::error_code ec;
    return DevFilesEnabled() && std::filesystem::exists(rel, ec) ? std::string(rel) : std::string();
}

std::string DataPath(const char* rel) {
    namespace fs = std::filesystem;
    std::string generated;
    const unsigned char* data = nullptr; size_t size = 0;
    if (!strcmp(rel, "game/data/textures/_MD_Flat_Normal_NML.dds")) { generated = FlatNormalDds(); data = (const unsigned char*)generated.data(); size = generated.size(); }
    else if (!strcmp(rel, "game/data/textures/_MD_Missing_DIF.dds")) { generated = MissingDiffuseDds(); data = (const unsigned char*)generated.data(); size = generated.size(); }
    else for (const EmbeddedFile& f : Table()) if (!strcmp(f.path, rel)) { data = f.data; size = f.size; break; }
    if (!data) return rel;                                            // not one of ours: the caller's own rules apply
    if (DevFilesEnabled()) { std::error_code ec; if (fs::exists(rel, ec)) return rel; }
    uint64_t key = Fnv(1469598103934665603ull, rel, strlen(rel));
    key = Fnv(key, &size, sizeof size);
    key = Fnv(key, data, size < 65536 ? size : 65536);
    key = Fnv(key, data + (size < 65536 ? 0 : size - 65536), size < 65536 ? size : 65536);
    const std::string name = fs::path(rel).filename().string();
    std::string p = CachePath(key, name.c_str());
    std::error_code ec;
    if (!p.empty() && fs::file_size(p, ec) == size && !ec) return p;   // already written
    if (!CacheWrite(key, name.c_str(), data, size)) return std::string();
    return p;
}

} // namespace md

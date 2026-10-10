#include <monkey_dust/io/kenshi_dir.h>
#include <monkey_dust/platform/md_log.h>

#include <cstdlib>
#include <filesystem>
#include <set>

namespace md {

namespace {
bool Exists(const std::string& path) { std::error_code ec; return std::filesystem::exists(path, ec); }
} // namespace

std::string KenshiDir() {
    if (const char* e = std::getenv("KENSHI_DIR")) if (*e) return e;
    if (Exists("tmp_/kenshi/data/gamedata.base")) return "tmp_/kenshi";
    return "";
}

std::string KenshiLandFile(const char* name, const char* fallback) {
    static std::set<std::string> logged;
    const std::string dir = KenshiDir();
    const std::string path = dir.empty() ? std::string() : dir + "/data/newland/land/" + name;
    const bool from_install = !path.empty() && Exists(path);
    if (logged.insert(name).second)
        MD_LOG(MD_LOG_INFO, "[Kenshi] %s: %s", name, from_install ? path.c_str() : fallback);
    return from_install ? path : std::string(fallback);
}

} // namespace md

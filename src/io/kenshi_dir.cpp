#include <monkey_dust/io/kenshi_dir.h>
#include <monkey_dust/platform/md_log.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <filesystem>
#include <iterator>
#include <set>

namespace md {

namespace {
bool Exists(const std::string& path) { std::error_code ec; return std::filesystem::exists(path, ec); }

bool IsKenshi(const std::string& dir) { return Exists(dir + "/data/gamedata.base"); }

std::string Home() { const char* h = std::getenv("HOME"); return h ? h : ""; }

std::string ReadAll(const std::string& path) {
    std::ifstream f(path);
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

std::string Trim(std::string s) { while (!s.empty() && std::isspace((unsigned char)s.back())) s.pop_back(); size_t a = 0; while (a < s.size() && std::isspace((unsigned char)s[a])) ++a; return s.substr(a); }
} // namespace

std::vector<std::string> FindSteamKenshi(const std::string& home) {
    // Steam roots: native (two usual places) and the Flatpak Steam
    const char* roots[] = { "/.steam/steam", "/.local/share/Steam", "/.var/app/com.valvesoftware.Steam/.local/share/Steam", "/.var/app/com.valvesoftware.Steam/.steam/steam" };
    std::vector<std::string> libs, found;
    auto add_lib = [&](const std::string& l) { if (std::find(libs.begin(), libs.end(), l) == libs.end()) libs.push_back(l); };
    for (const char* r : roots) {
        const std::string root = home + r;
        if (!Exists(root)) continue;
        add_lib(root);
        // libraryfolders.vdf lists the other library folders: lines like   "path"		"/mnt/games/SteamLibrary"
        const std::string vdf = ReadAll(root + "/steamapps/libraryfolders.vdf");
        size_t pos = 0;
        while ((pos = vdf.find("\"path\"", pos)) != std::string::npos) {
            pos += 6;
            const size_t a = vdf.find('"', pos), b = a == std::string::npos ? a : vdf.find('"', a + 1);
            if (b == std::string::npos) break;
            add_lib(vdf.substr(a + 1, b - a - 1));
            pos = b + 1;
        }
    }
    for (const std::string& l : libs) {
        const std::string k = l + "/steamapps/common/Kenshi";
        if (IsKenshi(k) && std::find(found.begin(), found.end(), k) == found.end()) found.push_back(k);
    }
    return found;
}

bool SaveKenshiDir(const std::string& dir, const std::string& home) {
    if (!IsKenshi(dir)) return false;
    std::error_code ec;
    std::filesystem::create_directories(home + "/.config/monkeydust", ec);
    std::ofstream f(home + "/.config/monkeydust/kenshi_dir", std::ios::trunc);
    if (!f) return false;
    f << dir << "\n";
    return (bool)f;
}

std::string SavedKenshiDir(const std::string& home) {
    std::string saved = Trim(ReadAll(home + "/.config/monkeydust/kenshi_dir"));
    if (saved.empty()) {   // saved under the old name (monkey_dust) before 23.10: take it over once, then only the new file counts
        saved = Trim(ReadAll(home + "/.config/monkey_dust/kenshi_dir"));
        if (!saved.empty() && IsKenshi(saved)) SaveKenshiDir(saved, home);
    }
    return !saved.empty() && IsKenshi(saved) ? saved : std::string();
}

bool IsKenshiFolder(const std::string& dir) { return IsKenshi(dir); }

static std::string& Override() { static std::string o; return o; }
void SetKenshiDir(const std::string& dir) { Override() = dir; }

bool KenshiFolderNeedsPrompt() {
    if (std::getenv("KENSHI_DIR")) return false;   // the environment decides (an empty value switches the install off on purpose)
#ifndef MD_NO_DEV_PATHS
    if (Exists("tmp_/kenshi/data/gamedata.base")) return false;
#endif
    const std::string home = Home();
    return home.empty() ? false : SavedKenshiDir(home).empty();
}

std::string DetectedKenshiDir() {
    const std::string home = Home();
    if (home.empty()) return {};
    const std::string saved = SavedKenshiDir(home);
    if (!saved.empty()) return saved;
    const std::vector<std::string> steam = FindSteamKenshi(home);
    return steam.empty() ? std::string() : steam.front();
}

std::string KenshiDir() {
    if (!Override().empty()) return Override();
    if (const char* e = std::getenv("KENSHI_DIR")) if (*e) return e;
#ifndef MD_NO_DEV_PATHS
    if (Exists("tmp_/kenshi/data/gamedata.base")) return "tmp_/kenshi";
#endif
    static const std::string found = [] {
        const std::string home = Home();
        if (home.empty()) return std::string();
        const std::string saved = SavedKenshiDir(home);
        if (!saved.empty()) return saved;
        const std::vector<std::string> steam = FindSteamKenshi(home);
        return steam.empty() ? std::string() : steam.front();
    }();
    return found;
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

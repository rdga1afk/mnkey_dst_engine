// Behaviour ported from Project Okran src/io/resources.rs (MIT, Copyright (c) 2026 brayniac6-glitch). See THIRD_PARTY.md.
#include <monkey_dust/io/kenshi_resources.h>
#include <algorithm>
#include <filesystem>
#include <vector>

namespace md {
namespace fs = std::filesystem;

namespace {
std::string Lower(std::string s) { for (char& c : s) c = char(tolower((unsigned char)c)); return s; }

void Walk(const fs::path& root, const fs::path& rel, std::unordered_map<std::string, std::string>& out) {
    std::error_code ec;
    std::vector<fs::directory_entry> entries;
    for (fs::directory_iterator it(root / rel, ec), end; !ec && it != end; it.increment(ec)) entries.push_back(*it);
    std::sort(entries.begin(), entries.end(), [](const fs::directory_entry& a, const fs::directory_entry& b) { return a.path().filename() < b.path().filename(); });
    for (const auto& e : entries) {
        fs::path r = rel / e.path().filename();
        if (e.is_directory(ec)) Walk(root, r, out);
        else out.emplace(Lower(e.path().filename().string()), r.generic_string());   // first found in sorted directory order
    }
}
} // namespace

std::string NormalizeKenshiPath(const std::string& stored) {
    std::string s = stored;
    for (const char* p : {".\\", "./"}) while (s.compare(0, 2, p) == 0) s.erase(0, 2);
    for (char& c : s) if (c == '\\') c = '/';
    return s;
}

void KenshiResources::Index(const std::string& root) {
    root_ = root;
    by_name_.clear();
    Walk(root, "data", by_name_);
}

std::string KenshiResources::Find(const std::string& stored) const {
    std::string rel = NormalizeKenshiPath(stored);
    std::error_code ec;
    if (!rel.empty() && fs::is_regular_file(fs::path(root_) / rel, ec)) return rel;
    size_t slash = rel.rfind('/');
    std::string name = Lower(slash == std::string::npos ? rel : rel.substr(slash + 1));
    auto it = by_name_.find(name);
    if (it != by_name_.end()) return it->second;
    return "";
}

std::string KenshiResources::FindTexture(const std::string& stored) const {
    std::string rel = Find(stored);
    if (!rel.empty()) return rel;
    std::string name = Lower(stored.substr(stored.find_last_of("/\\") == std::string::npos ? 0 : stored.find_last_of("/\\") + 1));
    size_t dot = name.rfind('.');
    if (dot == std::string::npos || dot < 3 || name.compare(dot - 3, 3, "_hi") != 0) return "";
    auto it = by_name_.find(name.substr(0, dot - 3) + name.substr(dot));
    return it == by_name_.end() ? "" : it->second;
}

std::string KenshiResources::Path(const std::string& stored) const {
    std::string rel = Find(stored);
    return rel.empty() ? rel : (fs::path(root_) / rel).string();
}

} // namespace md

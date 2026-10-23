#include <monkey_dust/io/kenshi_loading_screen.h>
#include <fstream>
#include <iterator>

namespace md::kenshi {

int PickIndex(int rand_value, int n) {
    if (n <= 0) return 0;
    const float f = (float)((double)rand_value * -0.000030517578125);   // -2^-15, the constant at 141742c38
    int i = 0 - (int)(f * (float)n);
    if (i > n - 1) i = n - 1;
    if (i < 0) i = 0;
    return i;
}

const std::string& LoadingScreenData::TextureFor(int i) const {
    static const std::string none;
    if (i < 0 || i >= (int)group_names.size()) return none;
    for (size_t k = 0; k < group_names.size(); ++k) if (group_names[k] == group_names[(size_t)i]) return group_textures[k];
    return none;
}

static std::string ReadAll(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

static std::string Attr(const std::string& tag, const char* key) {
    const std::string k = std::string(key) + "=\"";
    const size_t a = tag.find(k);
    if (a == std::string::npos) return "";
    const size_t b = tag.find('"', a + k.size());
    return b == std::string::npos ? "" : tag.substr(a + k.size(), b - a - k.size());
}

bool LoadLoadingScreenData(const std::string& root, LoadingScreenData& out) {
    out = LoadingScreenData{};
    const std::string xml = ReadAll(root + "/data/gui/images/kenshi_images.xml");
    const size_t set = xml.find("name=\"Kenshi_LoadingScreen\"");
    if (set == std::string::npos) return false;
    const size_t end = xml.find("</Resource>", set);
    for (size_t p = xml.find("<Group ", set); p != std::string::npos && p < end; p = xml.find("<Group ", p + 1)) {
        const std::string tag = xml.substr(p, xml.find('>', p) - p);
        out.group_names.push_back(Attr(tag, "name"));
        out.group_textures.push_back(Attr(tag, "texture"));
    }
    if (out.group_names.empty()) return false;
    std::ifstream t(root + "/data/tips.txt");
    std::string line;
    while (std::getline(t, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
        for (size_t q = line.find("\\n"); q != std::string::npos; q = line.find("\\n", q + 1)) line.replace(q, 2, "\n");
        if (!line.empty()) out.tips.push_back(line);
    }
    return true;
}

}  // namespace md::kenshi

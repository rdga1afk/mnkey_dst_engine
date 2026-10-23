#include <monkey_dust/world/overlay_mask_build.h>
#include <monkey_dust/io/derived_cache.h>
#include <stb_image.h>
#include <cstdio>
#include <cstring>

namespace md {
namespace {
constexpr int OVL_GRID = 8, TILE_OUT = OVERLAY_MASK_SIZE / OVL_GRID;
std::string TilePath(const std::string& root, int tx, int ty) {
    char p[512]; snprintf(p, sizeof(p), "%s/data/newland/land/overlaymaps/new_overlay.%d.%d.png", root.c_str(), tx, ty);
    return p;
}
}

bool BuildOverlayMask(const std::string& root, std::vector<uint8_t>& out) {
    out.assign((size_t)OVERLAY_MASK_SIZE * OVERLAY_MASK_SIZE * 4, 0);
    for (int ty = 0; ty < OVL_GRID; ++ty) for (int tx = 0; tx < OVL_GRID; ++tx) {
        int w = 0, h = 0, c = 0;
        uint8_t* t = stbi_load(TilePath(root, tx, ty).c_str(), &w, &h, &c, 4);
        if (!t) { fprintf(stderr, "[OverlayMask] missing tile %d.%d\n", tx, ty); out.clear(); return false; }
        for (int y = 0; y < TILE_OUT; ++y) for (int x = 0; x < TILE_OUT; ++x) {
            // PIL NEAREST: source pixel = floor((x + 0.5) * w / TILE_OUT)
            const int sx = (int)((x + 0.5) * w / TILE_OUT), sy = (int)((y + 0.5) * h / TILE_OUT);
            memcpy(&out[((size_t)(ty * TILE_OUT + y) * OVERLAY_MASK_SIZE + tx * TILE_OUT + x) * 4], t + ((size_t)sy * w + sx) * 4, 4);
        }
        stbi_image_free(t);
    }
    return true;
}

bool LoadOverlayMask(const std::string& root, std::vector<uint8_t>& out) {
    std::vector<std::string> files;
    for (int ty = 0; ty < OVL_GRID; ++ty) for (int tx = 0; tx < OVL_GRID; ++tx) files.push_back(TilePath(root, tx, ty));
    const uint64_t key = HashFiles(files);
    if (CacheRead(key, "overlay_mask.rgba", out) && out.size() == (size_t)OVERLAY_MASK_SIZE * OVERLAY_MASK_SIZE * 4) return true;
    if (!BuildOverlayMask(root, out)) return false;
    CacheWrite(key, "overlay_mask.rgba", out.data(), out.size());
    return true;
}
}

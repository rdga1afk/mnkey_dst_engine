#include <monkey_dust/world/kbi1.h>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace md {

bool Kbi1::Load(const std::string& path) {
    dim_ = res_ = block_ = 0; ids_.clear(); masks_.clear();
    FILE* f = fopen(path.c_str(), "rb"); if (!f) return false;
    char magic[4]; int32_t dim = 0, res = 0, block = 0;
    bool ok = fread(magic, 1, 4, f) == 4 && memcmp(magic, "KBI1", 4) == 0 && fread(&dim, 4, 1, f) == 1 && fread(&res, 4, 1, f) == 1 && dim > 0 && dim <= 256 && res > 0 && res <= 1024;
    if (ok) {
        ids_.resize((size_t)dim * dim);
        for (auto& c : ids_) ok = ok && fread(c.data(), 4, 5, f) == 5;
        ok = ok && fread(&block, 4, 1, f) == 1 && block > 0 && block <= (1 << 22);
    }
    if (ok) {
        masks_.resize((size_t)dim * dim);
        for (auto& m : masks_) {
            uint8_t b = 0; if (fread(&b, 1, 1, f) != 1) { ok = false; break; }
            if ((b & 0xe0) == 0x20) { m.assign(1, b); continue; }
            m.resize((size_t)block); m[0] = b;
            if (fread(m.data() + 1, 1, (size_t)block - 1, f) != (size_t)block - 1) { ok = false; break; }
        }
    }
    fclose(f);
    if (!ok) { ids_.clear(); masks_.clear(); return false; }
    dim_ = dim; res_ = res; block_ = block;
    return true;
}

int Kbi1::SubdividedCells() const { int n = 0; for (const auto& m : masks_) n += m.size() > 1; return n; }

static uint32_t Spread(uint32_t v) { v = (v | (v << 8)) & 0x00ff00ffu; v = (v | (v << 4)) & 0x0f0f0f0fu; v = (v | (v << 2)) & 0x33333333u; v = (v | (v << 1)) & 0x55555555u; return v; }

uint8_t Kbi1::LeafMask(int cell, int sx, int sz) const {
    const auto& m = masks_[(size_t)cell];
    if (m.size() == 1) return m[0];
    const uint32_t i = (uint32_t)(res_ * res_) | (Spread((uint32_t)sz) << 1) | Spread((uint32_t)sx);
    return i < m.size() ? m[i] : m[0];
}

PatchBiomes Kbi1::Query(float min_x, float min_z, float max_x, float max_z) const {
    PatchBiomes out;
    if (!Loaded()) return out;
    const float kMin = -147456.f, kSpan = 294912.f, d = (float)dim_;
    const float x0 = (min_x - kMin) / kSpan * d, z0 = (min_z - kMin) / kSpan * d;
    const float x1 = (float)((double)((max_x - kMin) / kSpan * d) - 1e-4), z1 = (float)((double)((max_z - kMin) / kSpan * d) - 1e-4);   // the original keeps the max corner inside its cell by a small offset
    const float cx0 = std::floor(x0), cz0 = std::floor(z0), cx1 = std::floor(x1), cz1 = std::floor(z1);
    if (cx0 != cx1 || cz0 != cz1 || cx0 < 0 || cz0 < 0 || cx0 >= d || cz0 >= d) return out;
    const int cell = (int)cz0 * dim_ + (int)cx0;
    const auto& m = masks_[(size_t)cell];
    uint8_t mask = m[0];
    if ((m[0] & 0xe0) > 0x20) {
        uint32_t a = (uint32_t)((x0 - cx0) * (float)res_), b = (uint32_t)((z0 - cz0) * (float)res_);
        uint32_t c = (uint32_t)((x1 - cx1) * (float)res_), e = (uint32_t)((z1 - cz1) * (float)res_);
        uint32_t i0 = (uint32_t)(res_ * res_) | (Spread(b) << 1) | Spread(a), i1 = (uint32_t)(res_ * res_) | (Spread(e) << 1) | Spread(c);
        while (i0 != i1) { i0 >>= 2; i1 >>= 2; }
        mask = i0 < m.size() ? m[i0] : m[0];
    }
    for (int slot = 4; slot >= 0 && out.count < 4; --slot) {
        if (!(mask & (1u << slot)) || ids_[(size_t)cell][(size_t)slot] == 0) continue;
        out.ids[out.count] = ids_[(size_t)cell][(size_t)slot]; out.channels[out.count] = slot; ++out.count;
    }
    return out;
}

}   // namespace md

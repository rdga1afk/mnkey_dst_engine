#include <monkey_dust/io/dds_decode.h>
#include <cstring>
#include <fstream>
#include <iterator>

namespace md {

namespace {
void Rgb565(uint16_t c, uint8_t o[3]) { o[0] = (uint8_t)(((c >> 11) & 31) * 255 / 31); o[1] = (uint8_t)(((c >> 5) & 63) * 255 / 63); o[2] = (uint8_t)((c & 31) * 255 / 31); }
// the 4 colours of a BC1 block; `opaque_mode` = BC3's colour half (always the 4-colour mode)
void Bc1Colours(const uint8_t* b, bool opaque_mode, uint8_t col[4][4]) {
    const uint16_t c0 = (uint16_t)(b[0] | b[1] << 8), c1 = (uint16_t)(b[2] | b[3] << 8);
    Rgb565(c0, col[0]); Rgb565(c1, col[1]); col[0][3] = col[1][3] = col[2][3] = 255; col[3][3] = 255;
    if (c0 > c1 || opaque_mode) { for (int k = 0; k < 3; ++k) { col[2][k] = (uint8_t)((2 * col[0][k] + col[1][k]) / 3); col[3][k] = (uint8_t)((col[0][k] + 2 * col[1][k]) / 3); } }
    else { for (int k = 0; k < 3; ++k) { col[2][k] = (uint8_t)((col[0][k] + col[1][k]) / 2); col[3][k] = 0; } col[3][3] = 0; }
}
}

bool DecodeDdsRgba(const std::string& path, int& w, int& h, std::vector<uint8_t>& out) {
    std::ifstream f(path, std::ios::binary);
    const std::vector<uint8_t> d((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (d.size() < 128 || memcmp(d.data(), "DDS ", 4) != 0) return false;
    auto u32 = [&](size_t o) { uint32_t v; memcpy(&v, d.data() + o, 4); return v; };
    h = (int)u32(12); w = (int)u32(16);
    const char* fcc = (const char*)d.data() + 84;
    size_t off = 128;
    if (w <= 0 || h <= 0) return false;
    out.assign((size_t)w * h * 4, 255);
    const bool bc1 = memcmp(fcc, "DXT1", 4) == 0, bc3 = memcmp(fcc, "DXT5", 4) == 0;
    if (bc1 || bc3) {
        const size_t bs = bc1 ? 8 : 16, bw = ((size_t)w + 3) / 4, bh = ((size_t)h + 3) / 4;
        if (d.size() < off + bw * bh * bs) return false;
        for (size_t by = 0; by < bh; ++by) for (size_t bx = 0; bx < bw; ++bx) {
            const uint8_t* b = &d[off + (by * bw + bx) * bs];
            uint8_t col[4][4]; Bc1Colours(b + (bc3 ? 8 : 0), bc3, col);
            const uint8_t* cb = b + (bc3 ? 8 : 0);
            uint8_t alpha[8] = {};
            if (bc3) { alpha[0] = b[0]; alpha[1] = b[1]; if (alpha[0] > alpha[1]) for (int k = 1; k < 7; ++k) alpha[k + 1] = (uint8_t)(((7 - k) * alpha[0] + k * alpha[1]) / 7); else { for (int k = 1; k < 5; ++k) alpha[k + 1] = (uint8_t)(((5 - k) * alpha[0] + k * alpha[1]) / 5); alpha[6] = 0; alpha[7] = 255; } }
            uint64_t abits = 0; if (bc3) for (int k = 0; k < 6; ++k) abits |= (uint64_t)b[2 + k] << (8 * k);
            for (int py = 0; py < 4; ++py) for (int px = 0; px < 4; ++px) {
                const size_t x = bx * 4 + px, y = by * 4 + py; if (x >= (size_t)w || y >= (size_t)h) continue;
                const int ci = (cb[4 + py] >> (2 * px)) & 3;
                uint8_t* o = &out[(y * w + x) * 4];
                o[0] = col[ci][0]; o[1] = col[ci][1]; o[2] = col[ci][2]; o[3] = bc3 ? alpha[(abits >> (3 * (py * 4 + px))) & 7] : col[ci][3];
            }
        }
        return true;
    }
    if (u32(88) == 32 && d.size() >= off + (size_t)w * h * 4) {   // uncompressed 32 bit, masks 0x00ff0000 0x0000ff00 0x000000ff (BGRA) or RGBA
        const bool bgra = u32(92) == 0x00ff0000u;
        for (size_t i = 0; i < (size_t)w * h; ++i) { const uint8_t* s = &d[off + i * 4]; out[i * 4] = bgra ? s[2] : s[0]; out[i * 4 + 1] = s[1]; out[i * 4 + 2] = bgra ? s[0] : s[2]; out[i * 4 + 3] = s[3]; }
        return true;
    }
    return false;
}

void BoxDownsample(std::vector<uint8_t>& rgba, int& w, int& h, int times) {
    for (int t = 0; t < times && w > 1 && h > 1; ++t) {
        const int nw = w / 2, nh = h / 2; std::vector<uint8_t> o((size_t)nw * nh * 4);
        for (int y = 0; y < nh; ++y) for (int x = 0; x < nw; ++x) for (int k = 0; k < 4; ++k) {
            const int s = rgba[((size_t)(2 * y) * w + 2 * x) * 4 + k] + rgba[((size_t)(2 * y) * w + 2 * x + 1) * 4 + k] + rgba[((size_t)(2 * y + 1) * w + 2 * x) * 4 + k] + rgba[((size_t)(2 * y + 1) * w + 2 * x + 1) * 4 + k];
            o[((size_t)y * nw + x) * 4 + k] = (uint8_t)((s + 2) / 4);
        }
        rgba.swap(o); w = nw; h = nh;
    }
}
}

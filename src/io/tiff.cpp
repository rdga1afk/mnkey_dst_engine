// Behaviour ported from Project Okran (MIT), src/io/tiff.rs -- see monkey_dust/io/tiff.h and THIRD_PARTY.md.
#include <monkey_dust/io/tiff.h>

#include <algorithm>
#include <cstring>

namespace md::tiff {
namespace {

constexpr uint16_t TAG_WIDTH = 256, TAG_HEIGHT = 257, TAG_BITS = 258, TAG_COMPRESSION = 259,
                   TAG_STRIP_OFFSETS = 273, TAG_SAMPLES = 277, TAG_ROWS_PER_STRIP = 278;

uint16_t U16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
uint32_t U32(const uint8_t* p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }

bool SeekTo(FILE* f, uint64_t at) { return fseeko(f, (off_t)at, SEEK_SET) == 0; }

} // namespace

void Image::Close() {
    if (file_) fclose(file_);
    file_ = nullptr;
    strips_.clear();
    width_ = height_ = bits_ = rows_per_strip_ = 0;
}

bool Image::Open(const char* path, std::string& error) {
    Close();
    file_ = fopen(path, "rb");
    if (!file_) { error = std::string("cannot open ") + path; return false; }
    uint8_t head[8];
    if (fread(head, 1, 8, file_) != 8 || memcmp(head, "II*\0", 4) != 0) {
        error = "not a little-endian TIFF"; Close(); return false;
    }
    uint8_t n2[2];
    if (!SeekTo(file_, U32(head + 4)) || fread(n2, 1, 2, file_) != 2) { error = "truncated IFD"; Close(); return false; }
    std::vector<uint8_t> entries((size_t)U16(n2) * 12);
    if (fread(entries.data(), 1, entries.size(), file_) != entries.size()) { error = "truncated IFD"; Close(); return false; }

    uint32_t compression = 1, samples = 1;
    uint32_t rows_per_strip = UINT32_MAX;
    bool have_strips = false;
    uint32_t strips_type = 0, strips_count = 0, strips_raw = 0;
    for (size_t i = 0; i + 12 <= entries.size(); i += 12) {
        const uint8_t* e = &entries[i];
        const uint16_t tag = U16(e), typ = U16(e + 2);
        const uint32_t count = U32(e + 4), raw = U32(e + 8);
        const uint32_t val = typ == 3 ? (raw & 0xffffu) : raw; // SHORT values sit in the low half of the value field
        switch (tag) {
            case TAG_WIDTH: width_ = val; break;
            case TAG_HEIGHT: height_ = val; break;
            case TAG_BITS: bits_ = val & 0xffffu; break;
            case TAG_COMPRESSION: compression = val; break;
            case TAG_SAMPLES: samples = val; break;
            case TAG_ROWS_PER_STRIP: rows_per_strip = val; break;
            case TAG_STRIP_OFFSETS: have_strips = true; strips_type = typ; strips_count = count; strips_raw = raw; break;
            default: break;
        }
    }
    if (compression != 1 || samples != 1 || !(bits_ == 8 || bits_ == 16)) {
        error = "unsupported TIFF: compression " + std::to_string(compression) + ", " + std::to_string(samples) +
                " samples, " + std::to_string(bits_) + " bits";
        Close(); return false;
    }
    if (!have_strips || strips_count == 0 || width_ == 0 || height_ == 0) { error = "unsupported TIFF: no strips"; Close(); return false; }
    if (strips_count == 1) {
        strips_.push_back(strips_raw);
    } else {
        // the field holds the offset of an array of LONG (4) or SHORT (3) values
        const size_t size = strips_type == 3 ? 2 : 4;
        std::vector<uint8_t> arr((size_t)strips_count * size);
        if (!SeekTo(file_, strips_raw) || fread(arr.data(), 1, arr.size(), file_) != arr.size()) {
            error = "truncated strip offsets"; Close(); return false;
        }
        for (size_t i = 0; i < strips_count; ++i) strips_.push_back(size == 2 ? U16(&arr[i * 2]) : U32(&arr[i * 4]));
    }
    rows_per_strip_ = std::min(rows_per_strip, std::max(height_, 1u));
    // every row must map to a listed strip
    if ((uint64_t)strips_.size() * rows_per_strip_ < height_) { error = "unsupported TIFF: strips do not cover the image"; Close(); return false; }
    return true;
}

uint64_t Image::RowOffset(uint32_t y) const {
    const uint32_t strip = y / rows_per_strip_;
    const uint64_t row_bytes = (uint64_t)width_ * (bits_ / 8);
    return strips_[strip] + (uint64_t)(y % rows_per_strip_) * row_bytes;
}

bool Image::Window(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t step, std::vector<uint16_t>& out, std::string& error) {
    if (!file_) { error = "no image open"; return false; }
    step = std::max(step, 1u);
    if (x >= width_ || y >= height_ || w == 0 || h == 0) { error = "window outside the image"; return false; }
    const uint32_t bpp = bits_ / 8;
    out.clear();
    out.reserve((size_t)w * h);
    const uint32_t span = std::min((w - 1) * step + 1, width_ - x);
    std::vector<uint8_t> row((size_t)span * bpp);
    for (uint32_t j = 0; j < h; ++j) {
        const uint32_t yy = std::min(y + j * step, height_ - 1);
        if (!SeekTo(file_, RowOffset(yy) + (uint64_t)x * bpp) || fread(row.data(), 1, row.size(), file_) != row.size()) {
            error = "truncated pixel data"; return false;
        }
        for (uint32_t i = 0; i < w; ++i) {
            const size_t xx = std::min((size_t)i * step, (size_t)span - 1);
            out.push_back(bpp == 2 ? U16(&row[xx * 2]) : (uint16_t)(row[xx] * 257u));
        }
    }
    return true;
}

} // namespace md::tiff

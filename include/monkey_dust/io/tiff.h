// Uncompressed greyscale TIFF, as used for Kenshi's terrain heightmap (`data/newland/land/fullmap.tif`): little-endian,
// one sample per pixel, 8 or 16 bits, no compression, strips. Pixels are read by seeking, so a window of a 537 MB map
// costs only that window. Nothing else is supported on purpose; any other file is refused with a reason.
//
// This is a format reader only: it contains no game data. LOAD TIME only (std::string / std::vector).
//
// Behaviour ported from Project Okran (https://github.com/brayniac6-glitch/Project-Okran, MIT licence):
// src/io/tiff.rs. Copyright (c) 2026 brayniac6-glitch. See THIRD_PARTY.md for the licence text.
#pragma once
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace md::tiff {

class Image {
public:
    Image() = default;
    ~Image() { Close(); }
    Image(const Image&) = delete;
    Image& operator=(const Image&) = delete;

    // false + `error` on failure (not found, not a little-endian TIFF, compressed, not greyscale 8/16 bit, no strips)
    bool Open(const char* path, std::string& error);
    void Close();

    uint32_t Width() const { return width_; }
    uint32_t Height() const { return height_; }
    uint32_t Bits() const { return bits_; }

    // `w` x `h` samples starting at (x, y), taken every `step` pixels, row-major, clamped to the image.
    // 8-bit values are widened to 16 bits (v * 257).
    bool Window(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t step, std::vector<uint16_t>& out,
                std::string& error);

private:
    uint64_t RowOffset(uint32_t y) const;

    FILE* file_ = nullptr;
    uint32_t width_ = 0, height_ = 0, bits_ = 0, rows_per_strip_ = 0;
    std::vector<uint64_t> strips_; // byte offset of each strip
};

} // namespace md::tiff

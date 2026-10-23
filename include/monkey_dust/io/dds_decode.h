// CPU decode of the top mip of a DDS file (BC1 / BC3 / uncompressed 32-bit) to RGBA8. LOAD TIME only.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace md {
bool DecodeDdsRgba(const std::string& path, int& w, int& h, std::vector<uint8_t>& rgba);
// Halves the image `times` times with a 2x2 box filter.
void BoxDownsample(std::vector<uint8_t>& rgba, int& w, int& h, int times);
}

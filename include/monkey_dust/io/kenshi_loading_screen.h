// The data and the picking rule of Kenshi's loading screen, read from the player's install (facts: docs/research/kenshi_specs/loading_screen.md in the private main).
// LOAD TIME only (std::string / std::vector).
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace md::kenshi {

// The C runtime rand() of the original exe (MSVC): 0..32767.
struct MsvcRand {
    uint32_t state;
    explicit MsvcRand(uint32_t seed = 1) : state(seed) {}
    int Next() { state = state * 214013u + 2531011u; return (int)((state >> 16) & 0x7fff); }
};

// Index in [0, n-1] from a rand() value, exactly as the exe's helper FUN_1409b3b40(0, n - 1): floor(rand / 32768 * n) in float arithmetic, clamped.
int PickIndex(int rand_value, int n);

struct LoadingScreenData {
    std::vector<std::string> group_names;      // the groups of the image set Kenshi_LoadingScreen in file order (a name may repeat)
    std::vector<std::string> group_textures;   // the texture of each group as written in the file
    std::vector<std::string> tips;             // lines of data/tips.txt, "\n" (backslash n) already turned into a line break
    // The picture shown for list index i: the exe sets the group BY NAME, so a repeated name resolves to the first group with that name.
    const std::string& TextureFor(int i) const;
};

// Reads <root>/data/gui/images/kenshi_images.xml and <root>/data/tips.txt. False when the image set is missing.
bool LoadLoadingScreenData(const std::string& root, LoadingScreenData& out);

}  // namespace md::kenshi

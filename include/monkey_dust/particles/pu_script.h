// ParticleUniverse (.pu) script reader: the script syntax of ParticleUniverse (MIT, Henry van Merode 2006-2013, OGRECave/particleuniverse) —
// a line is `keyword argument...`, optionally followed by a `{ ... }` block of further lines. Written new from the file format; see engine/THIRD_PARTY.md.
// Behaviour notes: docs/research/PARTICLES_INVENTORY_2026-11-15.md.
#pragma once
#include <string>
#include <vector>

namespace md::pu {

struct Node {
    std::string key;                  // first word of the line
    std::vector<std::string> args;    // the other words of the line
    std::vector<Node> children;       // the block that follows (empty when the line has none)
    int line = 0;                     // 1-based source line of the key
};

// Parses a whole script into its top-level nodes (`system`, `import` ...). false + `err` on an unbalanced or stray brace.
bool Parse(const std::string& text, std::vector<Node>& out, std::string& err);

// The first child with the key, or null.
const Node* Find(const Node& n, const std::string& key);

}  // namespace md::pu

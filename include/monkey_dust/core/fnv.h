#pragma once
#include <cstdint>

// FNV-1a 32-bit — compile-time and runtime key hashing.
// Used by AgentBlackboard (M18) and FlowGraph (M23).
// Replaces AI SHA1-based ShortGuid; single-pass, no deps.
namespace md {

constexpr uint32_t fnv1a(const char* s, uint32_t h = 2166136261u) noexcept {
    return *s ? fnv1a(s + 1, (h ^ static_cast<uint8_t>(*s)) * 16777619u) : h;
}

inline uint32_t fnv1a_rt(const char* s) noexcept {
    uint32_t h = 2166136261u;
    while (*s) { h = (h ^ static_cast<uint8_t>(*s++)) * 16777619u; }
    return h;
}

// Pattern 7: fnv_combine — hierarchical path hash.
// fnv_combine(parent, child) produces a stable hash for "parent.child" style paths
// without allocating a string. Used for nested entity/component references in FlowGraph.
constexpr uint32_t fnv_combine(uint32_t parent, uint32_t child) noexcept {
    // Feed the 4 bytes of child into the running hash seeded with parent
    uint32_t h = parent;
    h = (h ^ ((child      ) & 0xFFu)) * 16777619u;
    h = (h ^ ((child >>  8) & 0xFFu)) * 16777619u;
    h = (h ^ ((child >> 16) & 0xFFu)) * 16777619u;
    h = (h ^ ((child >> 24) & 0xFFu)) * 16777619u;
    return h;
}

// Convenience: hash a dotted path at compile time: fnv_path("entity", "component", "param")
constexpr uint32_t fnv_path(const char* a, const char* b) noexcept {
    return fnv_combine(fnv1a(a), fnv1a(b));
}
constexpr uint32_t fnv_path(const char* a, const char* b, const char* c) noexcept {
    return fnv_combine(fnv_path(a, b), fnv1a(c));
}

} // namespace md

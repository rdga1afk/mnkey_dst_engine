#pragma once
// RenderPassGraph — declarative render pass registry with resource dependency
// tracking. Intentionally a decl-and-toggle layer, not a full render graph —
// see docs/GRANITE_CONTINUATION_DECISION.md §3 for the scope decision.
//
// What this provides:
//   1. Enable/disable per pass — JSON override, runtime toggle, IsEnabled()
//      queried by every registered game pass (npc_render_*.cpp).
//   2. Resource declarations — each pass declares reads/writes of named
//      resources (name-hash tokens only, no live GPU handle). Used to
//      validate ordering and document data-flow between passes. SDL_GPU
//      handles actual VkImageLayout barriers internally.
//
// Resource access model:
//   DeclareRead(pass, resource)  — pass samples this texture as input
//   DeclareWrite(pass, resource) — pass renders into this texture
//
// Validation (Validate()):
//   For each pass P reading resource R, ensures the pass that last wrote R
//   is registered BEFORE P. Logs warnings for violations (soft — SDL_GPU
//   still handles barriers; violations are ordering intent mismatches).
//
// JSON format:
//   { "passes": { "shadow": true, "ssao": false } }
//
// MAX_PASSES = 32, MAX_RESOURCE_BINDINGS = 4 per pass.
//
// NOT implemented, by decision not oversight (docs/GRANITE_CONTINUATION_DECISION.md
// §3, 2026-08-30): automatic execution-order scheduling (RenderFrame's pass
// sequence stays hand-written) and a graph-owned live-texture pool replacing
// each *System's own SDL_GPUTexture* ownership. A GpuTexturePool-backed
// version of the latter was built and then removed 2026-09-03 (0 call sites
// anywhere) — this game's real per-frame barrier count is 1-2 (Forward) to
// 6-8 (Deferred_High), and transient-target memory is ~88MB against a ~4GB+
// UMA budget on the target hardware (Intel HD 520). Both are hand-tractable
// at this scale; the automation these two layers would buy is the same
// class of win Granite's real render_graph.cpp offers and was declined for
// the same reason. Revisit only if the "what could change this" conditions
// in that doc's §8 hold.

#include <cstdint>

namespace md {

// ── Resource access type ──────────────────────────────────────────────────────
enum class RGAccess : uint8_t {
    Read,        // sampled as texture input (e.g. shadow map read by main pass)
    Write,       // color / depth render target (exclusive write)
    ReadWrite,   // load + store (e.g. accumulate over existing content)
};

// Per-pass resource binding declaration.
struct RGResourceBinding {
    uint32_t resource_hash = 0;
    RGAccess access        = RGAccess::Read;
};

// ── Pass entry ────────────────────────────────────────────────────────────────
struct RenderPassEntry {
    char     name[32] = {};
    uint32_t hash     = 0;    // FNV-1a of name — O(1) hot-path lookup
    bool     enabled  = true;

    // Resource dependency declarations (up to MAX_RESOURCE_BINDINGS each).
    static constexpr int MAX_RESOURCE_BINDINGS = 4;
    RGResourceBinding reads [MAX_RESOURCE_BINDINGS] = {};
    RGResourceBinding writes[MAX_RESOURCE_BINDINGS] = {};
    int               read_count  = 0;
    int               write_count = 0;
};

// ── Singleton registry ────────────────────────────────────────────────────────
class RenderPassGraph {
public:
    static RenderPassGraph& Get();

    // 2026-10-08: 16 -> 32. At 16 the graph was already full (18 passes registered), Register() failed silently and IsEnabled() of the
    // unregistered 'evsm_shadow'/'skyvis_ao' stayed true, so md.set_render_pass_enabled() on them was a no-op (docs/FRAME_AUDIT_2026-09-27.md).
    static constexpr int MAX_PASSES = 32;

    // Register a pass. Returns false if MAX_PASSES exceeded or duplicate.
    bool Register(const char* name, bool default_enabled = true);

    // Declare resource access for ordering validation.
    // Call after Register(), before the render loop.
    // Unknown pass names are silently ignored (graceful degradation).
    void DeclareRead (const char* pass_name, const char* resource_name);
    void DeclareWrite(const char* pass_name, const char* resource_name);

    // Validate declared ordering: for each pass P reading resource R,
    // verifies the writing pass of R is registered before P.
    // Returns false and logs if violations found. Call after all declarations.
    bool Validate() const;

    // Load enable/disable overrides from "passes" section in JSON.
    // Non-existent file is a no-op; missing keys keep registered defaults.
    bool LoadFromJSON(const char* path);

    // Query enabled state — O(1) via FNV hash.
    bool IsEnabled(const char* name) const;
    bool IsEnabled(uint32_t name_hash) const;

    // Runtime toggle.
    void SetEnabled(const char* name, bool enabled);
    void SetEnabled(uint32_t name_hash, bool enabled);

    // Iteration.
    int                     PassCount()      const { return count_; }
    const RenderPassEntry&  GetPass(int idx) const { return passes_[idx]; }

    // Names/hashes already warned about by IsEnabled()/SetEnabled() on an unregistered pass (one log line each).
    // Fixed array, no heap; mutable because IsEnabled() is const.
    mutable uint32_t warned_unknown_[MAX_PASSES] = {};
    mutable int      warned_unknown_count_ = 0;
    void WarnUnknownOnce(const char* what, const char* name) const;

    // Reset to registered defaults.
    void Reset();

private:
    static uint32_t Hash(const char* s);
    int FindByHash(uint32_t h) const;

    RenderPassEntry passes_ [MAX_PASSES] = {};
    bool            defaults_[MAX_PASSES] = {};
    int             count_ = 0;
};

} // namespace md

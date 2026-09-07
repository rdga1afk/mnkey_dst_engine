#pragma once
#if defined(MD_ECS_GAIA)
#include <gaia.h>
#else
#include <flecs.h>
#endif
#include <cstdint>

// MdEntity — task #8 (EnTT->flecs strangler-fig migration), part B3.4.
//
// Backed by flecs::entity_t (raw uint64_t) now — was entt::entity through
// B1-B3.3. Construction from flecs::entity_t is EXPLICIT and there is no
// implicit conversion back — Raw() is the one blessed accessor.
//
// Null convention: MdEntity::Null() (id_=0), matching flecs's own
// invalid-entity value (unlike EnTT's all-1s convention). CORRECTION
// 2026-09-07: this comment previously claimed transform_soa.cpp's
// bulk-init memset fills 0x00 -- checked the real file, it fills 0xFF
// (transform_soa.cpp:23, "all = MdEntity::Null() pattern" -- that inline
// comment is ALSO inaccurate: 0xFF != MdEntity::Null()'s real id_=0).
// Harmless under flecs today only because flecs's entity_t never equals
// UINT64_MAX for a real entity, so the mismatched sentinel never
// collides -- not because the pattern is actually Null(). Under a
// gaia-ecs-backed MdEntity, 0xFF IS gaia's real EntityBad sentinel (see
// docs/GAIA_MIGRATION_ANALYSIS.md, UNKNOWN-1) -- the gaia backend's
// Null() is defined to match it, which makes this file's own comment
// finally true instead of incidentally harmless.
// entt::null_t and <entt/entt.hpp> were a deliberate B3.4 compatibility
// shim (kept so ~90 call sites using `entt::null` as a sentinel didn't
// need touching at the time) — removed entirely in the facade-removal
// pass (Phase 0.5): every former `entt::null` use is now `MdEntity::Null()`.
//
// MdEntity(uint32_t) reconstructs from just the low 32 bits (generation
// defaults to 0) — a "dumb", world-independent bit-reconstruction. flecs
// entity_t packs a 32-bit index in the low bits and a 16-bit generation
// (ECS_GENERATION_MASK = 0xFFFFull << 32, flecs.h) plus 4 ID-flag bits in
// the high bits; if the original entity died and its index got recycled by
// a newer entity, this constructor does NOT recover that — it's the raw,
// no-world-access fallback. MdRegistry::FromIndex(uint32_t) is the
// world-aware equivalent (via ecs_get_alive) and is what call sites that
// round-trip an entity through a uint32_t (BlackboardEntry::val.e, Lua
// integer args) should actually use.
#if defined(MD_ECS_GAIA)
// gaia-ecs backend. gaia::ecs::Entity is ALSO a 64-bit value type (union
// of a raw uint64 and a bit-packed struct: 32-bit index + 28-bit
// generation + 4 flag bits) -- same shape as flecs::entity_t, different
// split of the high 32 bits (verified P-F, docs/GAIA_MIGRATION_ANALYSIS.md
// UNKNOWN-1). Null() maps to gaia::ecs::EntityBad (all 64 bits 1) --
// gaia's OWN invalid-entity sentinel, NOT a translation of flecs's 0
// convention. See the class-level comment above: this actually makes
// transform_soa.cpp's memset(0xFF) pattern correct for the first time.
class MdEntity {
public:
    MdEntity() = default;
    explicit MdEntity(gaia::ecs::Entity e) : id_(e) {}
    // Zero-extends into the low 32 bits, matching gaia::ecs::Entity's
    // layout (index in the low 32 bits) exactly like the flecs branch --
    // verified via probe P-F, not assumed by symmetry.
    explicit MdEntity(uint32_t raw_index)
        : id_(static_cast<gaia::ecs::Identifier>(raw_index)) {}

    gaia::ecs::Entity Raw() const { return id_; }
    uint32_t          ToIntegral() const { return id_.id(); }

    static MdEntity Null() { return MdEntity(gaia::ecs::EntityBad); }

    friend bool operator==(MdEntity a, MdEntity b) { return a.id_ == b.id_; }
    friend bool operator!=(MdEntity a, MdEntity b) { return !(a.id_ == b.id_); }

private:
    gaia::ecs::Entity id_ = gaia::ecs::EntityBad;
};
#else
class MdEntity {
public:
    MdEntity() = default;
    explicit MdEntity(flecs::entity_t e) : id_(e) {}
    explicit MdEntity(uint32_t raw_index) : id_(static_cast<flecs::entity_t>(raw_index)) {}

    flecs::entity_t Raw() const { return id_; }
    uint32_t        ToIntegral() const { return static_cast<uint32_t>(id_); }

    static MdEntity Null() { return MdEntity(); }

    friend bool operator==(MdEntity a, MdEntity b) { return a.id_ == b.id_; }
    friend bool operator!=(MdEntity a, MdEntity b) { return a.id_ != b.id_; }

private:
    flecs::entity_t id_ = 0;
};
#endif

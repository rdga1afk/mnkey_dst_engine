#pragma once
#include <flecs.h>
#include <cstdint>

// MdEntity — task #8 (EnTT->flecs strangler-fig migration), part B3.4.
//
// Backed by flecs::entity_t (raw uint64_t) now — was entt::entity through
// B1-B3.3. Construction from flecs::entity_t is EXPLICIT and there is no
// implicit conversion back — Raw() is the one blessed accessor.
//
// Null convention: MdEntity::Null() (id_=0), matching flecs's own
// invalid-entity value (unlike EnTT's all-1s convention) — see
// transform_soa.cpp's bulk-init memset, which fills 0x00, not 0xFF.
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

#pragma once
//
// Adding a new component (in any .cpp):
//   static const FieldDesc wt_fields[] = {
//       MD_FIELD(WorldTransform, x,     F32),
//       MD_FIELD(WorldTransform, rot_y, F32),
//   };
//   ComponentReflect::Get().Register("world_transform",
//       wt_fields, 2, sizeof(WorldTransform));

#include <cstdint>
#include <cstddef>  // offsetof

namespace md {

// ── Field type tag ────────────────────────────────────────────────────────────
enum class FieldType : uint8_t {
    F32,  // float
    F64,  // double
    I8, I16, I32, I64,
    U8, U16, U32, U64,
    Bool,
    Vec3,  // float[3]
    Enum8, Enum16, Enum32,  // underlying integer; name printed as decimal
};

// ── Per-field metadata ────────────────────────────────────────────────────────
struct FieldDesc {
    const char* name;       // field name string literal
    FieldType   type;
    uint16_t    offset;     // offsetof(ComponentType, field)
    uint16_t    size;       // sizeof(field)
    // UI hints for F32 fields only (editor property panels). ui_min > ui_max
    // is the "unclamped" sentinel (a valid range always has min < max) —
    // MD_FIELD's default leaves these unset, so plain 4-arg registrations
    // stay unclamped with a sane default drag speed.
    float       ui_speed = 0.1f;
    float       ui_min   = 1.f;
    float       ui_max   = 0.f;
};

// ── Per-component metadata ────────────────────────────────────────────────────
struct ComponentDesc {
    char             name[32]  = {};
    uint32_t         name_hash = 0;
    const FieldDesc* fields    = nullptr;
    uint8_t          field_count    = 0;
    uint16_t         component_size = 0;
};

// ── Singleton registry ────────────────────────────────────────────────────────
class ComponentReflect {
public:
    static ComponentReflect& Get();

    static constexpr int MAX_COMPONENTS = 32;

    // Register a component type by name. Returns false on duplicate or full.
    // fields must outlive the registry (use file-scope static arrays).
    bool Register(const char* name,
                  const FieldDesc* fields, int field_count,
                  uint16_t component_size);

    // Look up by FNV-1a hash (fast path for hot loops).
    const ComponentDesc* Find(uint32_t name_hash) const;
    // Look up by name string.
    const ComponentDesc* Find(const char* name) const;

    int                  Count()        const { return count_; }
    const ComponentDesc& GetDesc(int i) const { return descs_[i]; }


private:
    static uint32_t Hash(const char* s);

    ComponentDesc descs_[MAX_COMPONENTS] = {};
    int           count_ = 0;
};

// ── Helper macro ─────────────────────────────────────────────────────────────
// Builds a FieldDesc entry from a struct type and field name.
// Usage: MD_FIELD(WorldTransform, x, F32)
#define MD_FIELD(StructType, field_name, ftype) \
    { #field_name, ::md::FieldType::ftype,       \
      (uint16_t)offsetof(StructType, field_name), \
      (uint16_t)sizeof(((StructType*)nullptr)->field_name) }

// Variant with explicit ImGui drag speed + clamp range (F32 fields).
// Usage: MD_FIELD_RANGE(WorldTransform, rot_y, F32, 1.f, -180.f, 180.f)
#define MD_FIELD_RANGE(StructType, field_name, ftype, speed, lo, hi) \
    { #field_name, ::md::FieldType::ftype,       \
      (uint16_t)offsetof(StructType, field_name), \
      (uint16_t)sizeof(((StructType*)nullptr)->field_name), \
      speed, lo, hi }

// ── Built-in registrations ────────────────────────────────────────────────────
// Registers engine-owned components (WorldTransform, SenseComponent, etc.).
// Call once at startup after ECS init.
void RegisterCoreComponents();

} // namespace md

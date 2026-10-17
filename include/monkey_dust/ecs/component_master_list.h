// Phase 5.1 (audit) — proof-of-concept, NOT wired into every component yet.
//
// Single source of truth for components needing flecs type registration
// (WarmUpEngineComponents(), component_warmup.cpp) — a real, minimal
// unification for 3 components picked because they're already registered
// identically in both WarmUpEngineComponents() AND RegisterCoreComponents()
// (component_reflect.cpp), so this list replaces their duplicate manual
// w.component<T>() lines with one generated block.
//
// NOT unified here: RegisterCoreComponents()'s per-FIELD reflection
// metadata (FieldDesc arrays — name/type/offset/ui-range per field) can't
// be reduced to a bare type list without ALSO encoding every field, which
// would need a bigger nested X-macro (e.g.
// MD_COMPONENT_FIELDS_BEGIN(WorldTransform) MD_FIELD_X(x, F32) ...
// MD_COMPONENT_FIELDS_END()) — a real, larger design, described in the
// audit report but not implemented here to keep this proof-of-concept
// minimal and low-risk.
//
// NOT unified here either: SaveSystem/WorldSerializer. It has no
// per-component registration concept at all — game/src/save/
// world_serializer.cpp hand-packs specific fields into NpcRecord/
// BuildingRecord binary structs (e.g. `rec.faction_id = ai.faction_id`),
// not a generic reflected serializer — folding it into this list would
// require redesigning the save format itself, a much bigger change than a
// 3-5 component proof-of-concept warrants.
//
// Usage:
//   #define MD_COMPONENT(CppType) w.component<CppType>();
//   #include <monkey_dust/ecs/component_master_list.h>
//   #undef MD_COMPONENT
#ifndef MD_COMPONENT
#error "Define MD_COMPONENT(CppType) before including component_master_list.h"
#endif

MD_COMPONENT(WorldTransform)
MD_COMPONENT(LimbHealth)

#undef MD_COMPONENT

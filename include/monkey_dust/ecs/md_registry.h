#pragma once
// md_registry.h — MdRegistry facade over gaia-ecs.
//
// Code audit proposal #12 (docs/CODE_AUDIT_2026-09.md, 2026-09): this file
// was ~948 lines / ~65 fan-in. Split into 4 files by real dependency order
// (each depends only on earlier ones, no back-references, verified before
// splitting -- not assumed):
//   md_registry_stage_scope.h    — MdRegistryStageScope, MdManagedTag
//   md_registry_query_helpers.h  — MdEach, MdFirst + their SFINAE-dispatch
//                                   machinery (gaia-ecs bug workarounds)
//   gaia_entity_handle.h         — GaiaEntityHandle
//   md_registry_core.h           — MdRegistry itself
//
// This header is now a facade: every one of the ~65 existing
// `#include <monkey_dust/ecs/md_registry.h>` call sites keeps working
// unchanged, in the SAME declaration order as the original single file.
// Extraction was byte-exact via `sed` line-range extraction, not retyped
// -- this file's dense SFINAE dispatch (working around several real
// gaia-ecs library bugs: Sparse-storage view crashes, an Entity+Sparse
// compile failure, reserved-bootstrap-entity false-positive query matches)
// is exactly the kind of code where a transcription slip could silently
// change dispatch behavior with no compile error to catch it.

#include <monkey_dust/ecs/md_registry_stage_scope.h>
#include <monkey_dust/ecs/md_registry_query_helpers.h>
#include <monkey_dust/ecs/gaia_entity_handle.h>
#include <monkey_dust/ecs/md_registry_core.h>

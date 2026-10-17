#pragma once
// EditorCmdRegistry — host-owned command registry (EDITOR_AUTOMATION_PLAN_v1.md
// Phase 0 falsification probe + Phase 1.1-1.2 real design). Storage lives
// here, in the host executable, NEVER inside libeditor_panels.so — the
// hot-reloadable panels module registers its commands on load and MUST
// unregister them on unload via UnregisterModule(), or dlclose leaves
// dangling function pointers behind (same invariant class as the Flecs
// structural-deferral rule).
//
// Header-only Meyer's singleton, same pattern as EditorPanelRegistry
// (editor_panel_registry.h) — proven precedent in this codebase for a
// registry shared correctly between the host .exe and the panels .so across
// a dlopen/dlclose cycle (inline function-local static, default ELF
// visibility de-duplicates the symbol across the two binaries).
//
// Undo wiring (Phase 1.2): EditorCmdRegistry lives in engine/; the actual
// undo/redo STACK (CommandStack, MAX_UNDO=64 ring buffer) lives in
// tools/editor/editor_history.h — editor-only, #ifdef MONKEY_DUST_EDITOR.
// engine/ must never #include tools/ (split-readiness invariant, CLAUDE.md),
// so Dispatch() does not push onto any stack itself. Instead it returns a
// DispatchOutcome carrying everything the caller needs (undo/redo fn
// pointers + the captured 64-byte blob); tools/ code builds its own
// CommandStack::Command from that and calls history.Push(). The 64-byte
// undo_data size deliberately matches CommandStack::Command::data[64] so
// the copy is a straight memcpy with no re-derivation of the payload size,
// and CmdUndoApplyFn/CmdRedoApplyFn intentionally share
// CommandStack::Command::undo/execute's exact `void(*)(void*, MdRegistry&)`
// shape for the same reason.
#include <monkey_dust/core/fnv.h>
#include <monkey_dust/ecs/md_registry.h>
#include <cstdint>
#include <cstdio>
#include <cstring>

enum class CmdArgType : uint8_t { I64, F64, Entity, Str };

// Fixed-capacity tagged value — no heap, no std::variant across the ABI
// boundary (panels .so / host executable).
struct CmdArgValue {
    CmdArgType type = CmdArgType::I64;
    union {
        int64_t  i64;
        double   f64;
        uint64_t entity_id; // raw flecs::entity_t
        char     str[64];
    };
    CmdArgValue() : i64(0) {}
};

struct CmdArgs {
    static constexpr int MAX_ARGS = 8;
    CmdArgValue values[MAX_ARGS];
    int count = 0;
};

// Every command returns something human/agent-readable.
struct CmdResult {
    bool ok = true;
    char msg[128] = {};
};

struct CmdArgDef {
    const char* name = nullptr;
    CmdArgType  type  = CmdArgType::I64;
};

// Per-command arg schema: powers introspection (help), validation, and
// agent discoverability. no_undo flags commands with no meaningful undo
// (Save, FPS query) so Dispatch() skips producing undo/redo data for them
// even if the caller happened to pass fn pointers.
struct CmdSchema {
    static constexpr int MAX_ARGS = 8;
    CmdArgDef args[MAX_ARGS] = {};
    int  count   = 0;
    bool no_undo = false;
};

// The command's real logic. undo_data is a 64-byte scratch buffer the
// command fills in IFF it supports undo (schema.no_undo == false) — same
// convention as CommandStack::Command::data, see file header.
using CmdExecuteFn = CmdResult (*)(const CmdArgs& args, MdRegistry& reg, uint8_t undo_data[64]);

// Matches CommandStack::Command::undo / ::execute exactly (undo and redo
// both operate on the captured 64-byte blob, not the original CmdArgs —
// redo must reproduce the SAME effect even if live state, e.g. selection,
// has since changed).
using CmdUndoApplyFn = void (*)(void* undo_data, MdRegistry& reg);
using CmdRedoApplyFn = void (*)(void* undo_data, MdRegistry& reg);

struct DispatchOutcome {
    CmdResult result;
    bool has_undo = false;
    CmdUndoApplyFn undo = nullptr;
    CmdRedoApplyFn redo = nullptr;
    uint8_t undo_data[64] = {};
};

class EditorCmdRegistry {
public:
    static constexpr int MAX_COMMANDS = 64;

    static EditorCmdRegistry& Get() {
        static EditorCmdRegistry inst;
        return inst;
    }

    // Registers a command owned by owner_module_id. Re-registration (same
    // name hash) updates execute/undo/redo/schema/owner in place. Silently
    // drops if MAX_COMMANDS is reached (no assert per project rules). undo
    // and redo may both be nullptr for a NO_UNDO command.
    bool Register(const char* name, CmdExecuteFn execute, CmdUndoApplyFn undo,
                  CmdRedoApplyFn redo, const CmdSchema& schema, uint32_t owner_module_id) {
        uint32_t h = md::fnv1a_rt(name);
        for (int i = 0; i < count_; ++i) {
            if (entries_[i].active && entries_[i].name_hash == h) {
                Entry& e = entries_[i];
                e.name = name; e.execute = execute; e.undo = undo; e.redo = redo;
                e.schema = schema; e.owner_module_id = owner_module_id;
                return true;
            }
        }
        if (count_ >= MAX_COMMANDS) return false;
        Entry& e = entries_[count_++];
        e.name_hash = h;
        e.name = name;
        e.execute = execute;
        e.undo = undo;
        e.redo = redo;
        e.schema = schema;
        e.owner_module_id = owner_module_id;
        e.active = true;
        return true;
    }

    // Purges every command owned by owner_module_id — call from the panels
    // module's shutdown entry point before dlclose. Compacts the array so
    // Count()/iteration never sees stale inactive holes.
    void UnregisterModule(uint32_t owner_module_id) {
        int w = 0;
        for (int r = 0; r < count_; ++r) {
            if (entries_[r].owner_module_id == owner_module_id) continue;
            if (w != r) entries_[w] = entries_[r];
            ++w;
        }
        count_ = w;
    }

    // Validates args against the command's schema, calls execute(), and —
    // if the command isn't NO_UNDO and registered both an undo and redo fn —
    // fills in the outcome's undo/redo/undo_data so the caller can push its
    // own CommandStack::Command. Never touches any undo stack itself (see
    // file header).
    DispatchOutcome Dispatch(const char* name, const CmdArgs& args, MdRegistry& reg) const {
        DispatchOutcome out;
        const Entry* e = Find(name);
        if (!e) {
            out.result.ok = false;
            snprintf(out.result.msg, sizeof(out.result.msg), "Unknown command: %s", name);
            return out;
        }
        if (!ValidateArgs(e->schema, args)) {
            out.result.ok = false;
            snprintf(out.result.msg, sizeof(out.result.msg),
                     "Arg mismatch for %s (expected %d arg(s))", name, e->schema.count);
            return out;
        }
        out.result = e->execute(args, reg, out.undo_data);
        if (out.result.ok && !e->schema.no_undo && e->undo && e->redo) {
            out.has_undo = true;
            out.undo = e->undo;
            out.redo = e->redo;
        }
        return out;
    }

    // Introspection: schema for `help`, name enumeration for `list`.
    const CmdSchema* Help(const char* name) const {
        const Entry* e = Find(name);
        return e ? &e->schema : nullptr;
    }

    int Count() const { return count_; }

    const char* NameAt(int i) const {
        return (i >= 0 && i < count_) ? entries_[i].name : nullptr;
    }

    // Test-only: full reset (mirrors EditorPanelRegistry::Clear()).
    void Clear() { count_ = 0; }

private:
    EditorCmdRegistry() = default;

    struct Entry {
        uint32_t name_hash = 0;
        const char* name = nullptr;
        CmdSchema schema;
        CmdExecuteFn execute = nullptr;
        CmdUndoApplyFn undo = nullptr;
        CmdRedoApplyFn redo = nullptr;
        uint32_t owner_module_id = 0;
        bool active = false;
    };

    const Entry* Find(const char* name) const {
        uint32_t h = md::fnv1a_rt(name);
        for (int i = 0; i < count_; ++i)
            if (entries_[i].active && entries_[i].name_hash == h) return &entries_[i];
        return nullptr;
    }

    static bool ValidateArgs(const CmdSchema& schema, const CmdArgs& args) {
        if (args.count != schema.count) return false;
        for (int i = 0; i < schema.count; ++i)
            if (args.values[i].type != schema.args[i].type) return false;
        return true;
    }

    Entry entries_[MAX_COMMANDS];
    int count_ = 0;
};

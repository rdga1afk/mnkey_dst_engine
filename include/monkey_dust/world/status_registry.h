#pragma once
#include <monkey_dust/core/fnv.h>
#include <cstring>

// ── MdStatusRegistry ─────────────────────────────────────────────────────────
// Flare CampaignManager-inspired string-based status flag store.
// Complements QuestSystem::IsBitSet (bitmask, 127 quests) with human-readable
// named statuses for richer save files and mod-author-friendly conditions.
//
// Registration: any code can call Register() at startup; idempotent.
// Status IDs: FNV-1a hash of the name — same hash for same string = stable.
//
// Save/load:
//   char csv[4096];
//   MdStatusRegistry::Get().GetAllCSV(csv, sizeof(csv));
//   // store csv in save file, restore with:
//   MdStatusRegistry::Get().SetAllCSV(csv);
//
// Usage:
//   uint32_t sid = MdStatusRegistry::Get().Register("kill_guard_captain");
//   MdStatusRegistry::Get().Set(sid);
//   if (MdStatusRegistry::Get().Check(sid)) { /* unlock shortcut */ }

static constexpr int MD_STATUS_MAX      = 128;
static constexpr int MD_STATUS_NAME_LEN = 48;

struct MdStatus {
    uint32_t hash              = 0;
    bool     active            = false;
    char     name[MD_STATUS_NAME_LEN] = {};
};

class MdStatusRegistry {
public:
    static MdStatusRegistry& Get() {
        static MdStatusRegistry inst;
        return inst;
    }

    // Register a named status (idempotent). Returns its FNV-1a hash.
    // Registering an unknown name creates a new slot; existing → returns hash.
    uint32_t Register(const char* name) noexcept {
        if (!name || !name[0]) return 0;
        uint32_t h = md::fnv1a_rt(name);
        for (int i = 0; i < count_; ++i)
            if (entries_[i].hash == h) return h;
        if (count_ >= MD_STATUS_MAX) return 0;
        MdStatus& e = entries_[count_++];
        e.hash   = h;
        e.active = false;
        int n = (int)strlen(name);
        if (n >= MD_STATUS_NAME_LEN) n = MD_STATUS_NAME_LEN - 1;
        memcpy(e.name, name, (size_t)n);
        e.name[n] = '\0';
        return h;
    }

    // Look up hash by name without registering. Returns 0 if not found.
    uint32_t Find(const char* name) const noexcept {
        if (!name || !name[0]) return 0;
        uint32_t h = md::fnv1a_rt(name);
        for (int i = 0; i < count_; ++i)
            if (entries_[i].hash == h) return h;
        return 0;
    }

    void Set   (uint32_t id) noexcept { if (auto* e = find(id)) e->active = true;  }
    void Unset (uint32_t id) noexcept { if (auto* e = find(id)) e->active = false; }
    void Toggle(uint32_t id) noexcept { if (auto* e = find(id)) e->active = !e->active; }
    bool Check (uint32_t id) const noexcept {
        const auto* e = find_const(id);
        return e && e->active;
    }

    // Convenience: register + set in one call.
    uint32_t SetByName(const char* name) noexcept {
        uint32_t id = Register(name);
        Set(id);
        return id;
    }
    bool CheckByName(const char* name) const noexcept {
        return Check(md::fnv1a_rt(name));
    }

    // Serialize active statuses to CSV (comma-separated names).
    // Returns bytes written (excluding null terminator).
    int GetAllCSV(char* out, int out_sz) const noexcept {
        if (!out || out_sz <= 0) return 0;
        int pos = 0;
        for (int i = 0; i < count_; ++i) {
            if (!entries_[i].active) continue;
            const char* nm = entries_[i].name;
            int n = (int)strlen(nm);
            if (pos + n + 2 > out_sz) break;  // +2 for comma + null
            if (pos > 0) out[pos++] = ',';
            memcpy(out + pos, nm, (size_t)n);
            pos += n;
        }
        out[pos] = '\0';
        return pos;
    }

    // Deserialize CSV: activate all statuses whose names appear in csv.
    // Statuses NOT in csv are unaffected (use Clear() first for full reset).
    void SetAllCSV(const char* csv) noexcept {
        if (!csv || !csv[0]) return;
        char token[MD_STATUS_NAME_LEN];
        const char* p = csv;
        while (*p) {
            // Extract token up to next comma or end
            int n = 0;
            while (*p && *p != ',' && n < MD_STATUS_NAME_LEN - 1)
                token[n++] = *p++;
            token[n] = '\0';
            if (*p == ',') ++p;
            if (n > 0) {
                uint32_t h = md::fnv1a_rt(token);
                // Register if unknown (save may have statuses not yet registered)
                Register(token);
                Set(h);
            }
        }
    }

    void Clear()  noexcept { count_ = 0; }
    int  Count()  const noexcept { return count_; }

    // Count only active statuses.
    int  ActiveCount() const noexcept {
        int n = 0;
        for (int i = 0; i < count_; ++i) n += entries_[i].active ? 1 : 0;
        return n;
    }

private:
    MdStatusRegistry() = default;

    MdStatus* find(uint32_t id) noexcept {
        for (int i = 0; i < count_; ++i)
            if (entries_[i].hash == id) return &entries_[i];
        return nullptr;
    }
    const MdStatus* find_const(uint32_t id) const noexcept {
        for (int i = 0; i < count_; ++i)
            if (entries_[i].hash == id) return &entries_[i];
        return nullptr;
    }

    MdStatus entries_[MD_STATUS_MAX] = {};
    int      count_ = 0;
};

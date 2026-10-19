#pragma once

// ── MdIniReader ───────────────────────────────────────────────────────────────
// zelda3-inspired (snesrev/zelda3 config.c) minimal INI parser.
// No malloc — fixed flat array of entries, parsed from a caller-owned buffer.
//
// Supported syntax:
//   [SectionName]          — section header (case-insensitive lookup)
//   key = value            — key-value pair (whitespace stripped)
//   # comment              — ignored line
//   ; comment              — ignored line
//   !include other.ini     — recursive include (depth limit = 4)
//
// File loading:
//   Fallback chain: monkey_dust.user.ini → monkey_dust.ini
//   Use MdIniLoad() which handles both file read and parse.
//
// Usage:
//   MdIniReader ini;
//   MdIniLoad(ini, "data/monkey_dust.ini");
//   int w = ini.GetInt("Graphics", "window_width", 1280);
//   bool fs = ini.GetBool("Graphics", "fullscreen", false);

static constexpr int MD_INI_MAX_ENTRIES = 256;
static constexpr int MD_INI_KEY_LEN     = 64;
static constexpr int MD_INI_VAL_LEN     = 256;
static constexpr int MD_INI_SEC_LEN     = 64;

struct MdIniEntry {
    char section[MD_INI_SEC_LEN] = {};
    char key    [MD_INI_KEY_LEN] = {};
    char value  [MD_INI_VAL_LEN] = {};
};

class MdIniReader {
public:
    // Parse INI from a mutable buffer (modified in-place for line splitting).
    // Returns number of entries parsed.
    int ParseBuffer(char* buf, int len, int include_depth = 0) noexcept;

    // Typed accessors — return def if key not found.
    const char* GetStr  (const char* section, const char* key,
                         const char* def = "")    const noexcept;
    int         GetInt  (const char* section, const char* key,
                         int def = 0)             const noexcept;
    float       GetFloat(const char* section, const char* key,
                         float def = 0.0f)        const noexcept;
    bool        GetBool (const char* section, const char* key,
                         bool def = false)         const noexcept;

    void Clear() noexcept { count_ = 0; }
    int  Count() const noexcept { return count_; }

private:
    // Case-insensitive string comparison (section/key names).
    static bool ieq(const char* a, const char* b) noexcept;
    // Strip leading/trailing whitespace in-place; returns trimmed start.
    static char* trim(char* s) noexcept;
    // ParseBool: "0/false/no/off" → false; "1/true/yes/on" → true.
    static bool  parse_bool(const char* s, bool def) noexcept;

    MdIniEntry entries_[MD_INI_MAX_ENTRIES] = {};
    int        count_ = 0;
};

// Load helper: reads file via md::fs_read_alloc, parses, frees buffer.
// Tries path first; if not found, does nothing (leaves reader unchanged).
// Returns number of entries parsed (-1 on file-not-found).
int MdIniLoad(MdIniReader& reader, const char* path) noexcept;


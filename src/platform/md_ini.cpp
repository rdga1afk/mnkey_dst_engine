#include <monkey_dust/platform/md_ini.h>
#include <monkey_dust/platform/md_fs.h>
#include <monkey_dust/platform/md_log.h>
#include <cstring>
#include <cstdlib>
#include <cstdio>

// ── Internal helpers ──────────────────────────────────────────────────────────

bool MdIniReader::ieq(const char* a, const char* b) noexcept {
    while (*a && *b) {
        char ca = (*a >= 'A' && *a <= 'Z') ? (*a + 32) : *a;
        char cb = (*b >= 'A' && *b <= 'Z') ? (*b + 32) : *b;
        if (ca != cb) return false;
        ++a; ++b;
    }
    return *a == '\0' && *b == '\0';
}

char* MdIniReader::trim(char* s) noexcept {
    while (*s == ' ' || *s == '\t') ++s;
    char* end = s + strlen(s);
    while (end > s && (end[-1] == ' ' || end[-1] == '\t' ||
                       end[-1] == '\r' || end[-1] == '\n'))
        *--end = '\0';
    return s;
}

bool MdIniReader::parse_bool(const char* s, bool def) noexcept {
    if (!s || !*s) return def;
    char c = (*s >= 'A' && *s <= 'Z') ? (*s + 32) : *s;
    switch (c) {
        case '0': case 'f': case 'n': return false;
        case '1': case 't': case 'y': return true;
        case 'o': // "on" / "off"
            return (s[1] == 'n' || s[1] == 'N');
    }
    return def;
}

// ── MdIniReader::ParseBuffer ──────────────────────────────────────────────────

int MdIniReader::ParseBuffer(char* buf, int /*len*/, int include_depth) noexcept {
    if (!buf || include_depth > 4) return 0;

    char cur_section[MD_INI_SEC_LEN] = {};
    int  parsed = 0;
    char* p = buf;

    while (*p) {
        // Find end of line
        char* line_start = p;
        while (*p && *p != '\n' && *p != '\r') ++p;
        if (*p == '\r') *p++ = '\0';
        if (*p == '\n') *p++ = '\0';
        else            *line_start = '\0'; // EOF without newline — still process

        char* line = trim(line_start);

        // Skip blank / comment lines
        if (!*line || *line == '#' || *line == ';') continue;

        // Section header: [SectionName]
        if (*line == '[') {
            char* end = strchr(line + 1, ']');
            if (end) {
                *end = '\0';
                char* sec = trim(line + 1);
                int n = (int)strlen(sec);
                if (n > 0 && n < MD_INI_SEC_LEN) {
                    memcpy(cur_section, sec, (size_t)n + 1);
                }
            }
            continue;
        }

        // !include directive
        if (strncmp(line, "!include", 8) == 0 && (line[8] == ' ' || line[8] == '\t')) {
            char* inc_path = trim(line + 9);
            if (*inc_path) {
                uint32_t fsz = 0;
                char* inc_buf = md::fs_read_alloc(inc_path, &fsz);
                if (inc_buf) {
                    parsed += ParseBuffer(inc_buf, (int)fsz, include_depth + 1);
                    md::fs_free(inc_buf);
                } else {
                    MD_LOG(MD_LOG_WARNING, "[MdIni] !include not found: %s", inc_path);
                }
            }
            continue;
        }

        // key = value
        char* eq = strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';
        char* key = trim(line);
        char* val = trim(eq + 1);

        if (!*key) continue;

        // Store entry
        if (count_ >= MD_INI_MAX_ENTRIES) {
            MD_LOG(MD_LOG_WARNING, "[MdIni] entry limit reached (%d)", MD_INI_MAX_ENTRIES);
            break;
        }
        MdIniEntry& e = entries_[count_++];
        strncpy(e.section, cur_section, MD_INI_SEC_LEN - 1);
        strncpy(e.key,     key,         MD_INI_KEY_LEN - 1);
        strncpy(e.value,   val,         MD_INI_VAL_LEN - 1);
        ++parsed;
    }

    return parsed;
}

// ── Typed accessors ───────────────────────────────────────────────────────────

const char* MdIniReader::GetStr(const char* section, const char* key,
                                 const char* def) const noexcept {
    for (int i = 0; i < count_; ++i)
        if (ieq(entries_[i].section, section) && ieq(entries_[i].key, key))
            return entries_[i].value;
    return def;
}

int MdIniReader::GetInt(const char* section, const char* key, int def) const noexcept {
    const char* v = GetStr(section, key, nullptr);
    return v ? (int)strtol(v, nullptr, 10) : def;
}

float MdIniReader::GetFloat(const char* section, const char* key,
                              float def) const noexcept {
    const char* v = GetStr(section, key, nullptr);
    return v ? (float)strtod(v, nullptr) : def;
}

bool MdIniReader::GetBool(const char* section, const char* key,
                           bool def) const noexcept {
    const char* v = GetStr(section, key, nullptr);
    return v ? parse_bool(v, def) : def;
}

// ── File loaders ──────────────────────────────────────────────────────────────

int MdIniLoad(MdIniReader& reader, const char* path) noexcept {
    uint32_t fsz = 0;
    char* buf = md::fs_read_alloc(path, &fsz);
    if (!buf) return -1;
    int n = reader.ParseBuffer(buf, (int)fsz);
    md::fs_free(buf);
    MD_LOG(MD_LOG_INFO, "[MdIni] loaded %d entries from %s", n, path);
    return n;
}


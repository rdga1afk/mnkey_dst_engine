// FCS reader. Behaviour ported from Project Okran src/io/fcs.rs (MIT, Copyright (c) 2026 brayniac6-glitch), see THIRD_PARTY.md.
#include <monkey_dust/io/fcs.h>
#include <cstdio>
#include <cstring>
#include <limits>

namespace md::fcs {

std::string Error::Message() const {
    char b[256];
    switch (kind) {
        case ErrorKind::None: return "ok";
        case ErrorKind::Io: return what;
        case ErrorKind::Truncated: snprintf(b, sizeof b, "truncated reading %s at byte %zu", what.c_str(), at); return b;
        case ErrorKind::UnsupportedFiletype: snprintf(b, sizeof b, "unsupported FCS filetype %lld", (long long)value); return b;
        case ErrorKind::BadLength: snprintf(b, sizeof b, "bad length %lld at byte %zu", (long long)value, at); return b;
        case ErrorKind::TrailingBytes: snprintf(b, sizeof b, "%zu trailing bytes after byte %zu", extra, at); return b;
        case ErrorKind::LengthMismatch: snprintf(b, sizeof b, "length %lld declared at byte %zu, parsed %zu", (long long)value, at, extra); return b;
    }
    return "?";
}

namespace {

struct Reader {
    const uint8_t* buf;
    size_t size;
    size_t pos = 0;
    Error* err;

    bool Fail(ErrorKind k, size_t at, const char* what, int64_t value = 0, size_t extra = 0) {
        if (err->kind == ErrorKind::None) { err->kind = k; err->at = at; err->what = what ? what : ""; err->value = value; err->extra = extra; }
        return false;
    }
    bool Take(size_t n, const char* what, const uint8_t*& out) {
        if (n > size - pos) return Fail(ErrorKind::Truncated, pos, what);
        out = buf + pos; pos += n; return true;
    }
    bool I32(const char* what, int32_t& v) {
        const uint8_t* p; if (!Take(4, what, p)) return false;
        uint32_t u = (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
        v = (int32_t)u; return true;
    }
    bool U32(const char* what, uint32_t& v) { int32_t i; if (!I32(what, i)) return false; v = (uint32_t)i; return true; }
    bool F32(const char* what, float& v) {
        const uint8_t* p; if (!Take(4, what, p)) return false;
        memcpy(&v, p, 4); return true; // little-endian host assumed (x86/ARM64 Linux/Windows)
    }
    // A count of items each at least `min_item` bytes long; rejects counts the file can't hold.
    bool Count(size_t min_item, const char* what, size_t& n) {
        size_t at = pos; int32_t v; if (!I32(what, v)) return false;
        size_t left = size - pos;
        if (v < 0 || (size_t)v > left / (min_item ? min_item : 1)) return Fail(ErrorKind::BadLength, at, what, v);
        n = (size_t)v; return true;
    }
    bool Str(const char* what, std::string& s) {
        size_t n; if (!Count(1, what, n)) return false;
        const uint8_t* p; if (!Take(n, what, p)) return false;
        s.assign((const char*)p, n); return true;
    }
};

bool ReadHeader(Reader& r, Header& h) {
    if (!r.I32("filetype", h.filetype)) return false;
    bool v17;
    if (h.filetype == FILETYPE_V15) {
        // v15 (zone/level files): no version or description strings, straight to the counts
        return r.I32("id_counter", h.id_counter) && r.I32("record_count", h.record_count);
    } else if (h.filetype == FILETYPE_V16) v17 = false;
    else if (h.filetype == FILETYPE_V17) v17 = true;
    else return r.Fail(ErrorKind::UnsupportedFiletype, r.pos, "filetype", h.filetype);
    size_t size_at = r.pos, size = 0;
    if (v17 && !r.Count(1, "header_size", size)) return false;
    size_t start = r.pos;
    if (!r.I32("version", h.version) || !r.Str("author", h.author) || !r.Str("description", h.description) ||
        !r.Str("dependencies", h.dependencies) || !r.Str("references", h.references)) return false;
    if (v17) {
        size_t used = r.pos - start;
        if (used > size) return r.Fail(ErrorKind::LengthMismatch, size_at, "header", (int64_t)size, used);
        const uint8_t* p; if (!r.Take(size - used, "header tail", p)) return false;
        h.tail.assign(p, p + (size - used));
    }
    return r.I32("id_counter", h.id_counter) && r.I32("record_count", h.record_count);
}

// (key, value) lists: `item` reads one value
template <class T, class F>
bool ReadList(Reader& r, size_t min_item, const char* what, std::vector<std::pair<std::string, T>>& out, F item) {
    size_t n; if (!r.Count(min_item, what, n)) return false;
    out.reserve(n);
    for (size_t i = 0; i < n; ++i) {
        std::string k; if (!r.Str(what, k)) return false;
        T v{}; if (!item(r, v)) return false;
        out.emplace_back(std::move(k), std::move(v));
    }
    return true;
}

bool ReadRecord(Reader& r, int32_t filetype, Record& rec) {
    size_t start = r.pos;
    if (!r.I32("record lead", rec.lead) || !r.I32("record type", rec.kind) || !r.I32("record id", rec.id) ||
        !r.Str("record name", rec.name) || !r.Str("record string_id", rec.string_id) || !r.U32("record datatype", rec.datatype))
        return false;
    if (!ReadList<bool>(r, 5, "bools", rec.bools, [](Reader& r, bool& v) { const uint8_t* p; if (!r.Take(1, "bool", p)) return false; v = p[0] != 0; return true; })) return false;
    if (!ReadList<float>(r, 8, "floats", rec.floats, [](Reader& r, float& v) { return r.F32("float", v); })) return false;
    if (!ReadList<int32_t>(r, 8, "ints", rec.ints, [](Reader& r, int32_t& v) { return r.I32("int", v); })) return false;
    if (!ReadList<std::array<float, 3>>(r, 16, "vec3s", rec.vec3s, [](Reader& r, std::array<float, 3>& v) { for (float& x : v) if (!r.F32("vec3", x)) return false; return true; })) return false;
    if (!ReadList<std::array<float, 4>>(r, 20, "vec4s", rec.vec4s, [](Reader& r, std::array<float, 4>& v) { for (float& x : v) if (!r.F32("vec4", x)) return false; return true; })) return false;
    if (!ReadList<std::string>(r, 8, "strings", rec.strings, [](Reader& r, std::string& v) { return r.Str("string", v); })) return false;
    if (!ReadList<std::string>(r, 8, "files", rec.files, [](Reader& r, std::string& v) { return r.Str("file", v); })) return false;
    size_t nc; if (!r.Count(8, "reference categories", nc)) return false;
    rec.references.reserve(nc);
    for (size_t i = 0; i < nc; ++i) {
        std::string cat; if (!r.Str("reference category", cat)) return false;
        size_t nr; if (!r.Count(16, "references", nr)) return false;
        std::vector<Reference> refs; refs.reserve(nr);
        for (size_t j = 0; j < nr; ++j) {
            Reference ref;
            if (!r.Str("reference target", ref.target) || !r.I32("ref val0", ref.values[0]) || !r.I32("ref val1", ref.values[1]) || !r.I32("ref val2", ref.values[2])) return false;
            refs.push_back(std::move(ref));
        }
        rec.references.emplace_back(std::move(cat), std::move(refs));
    }
    size_t ni; if (!r.Count(40, "instances", ni)) return false;
    rec.instances.reserve(ni);
    for (size_t i = 0; i < ni; ++i) {
        Instance in;
        if (!r.Str("instance id", in.id) || !r.Str("instance target", in.target)) return false;
        for (float& x : in.position) if (!r.F32("instance position", x)) return false;
        for (float& x : in.rotation) if (!r.F32("instance rotation", x)) return false;
        size_t ns; if (!r.Count(4, "instance states", ns)) return false;
        in.states.resize(ns);
        for (std::string& s : in.states) if (!r.Str("instance state", s)) return false;
        rec.instances.push_back(std::move(in));
    }
    size_t parsed = r.pos - start;
    // only v17 stores the record length in the lead (v15 zone files keep something else there)
    if (filetype == FILETYPE_V17 && rec.lead != 0 && (size_t)rec.lead != parsed)
        return r.Fail(ErrorKind::LengthMismatch, start, "record", rec.lead, parsed);
    return true;
}

bool ReadBytes(const std::string& path, std::vector<uint8_t>& out, Error& err) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) { err.kind = ErrorKind::Io; err.what = path + ": cannot open"; return false; }
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    if (n < 0) { fclose(f); err.kind = ErrorKind::Io; err.what = path + ": cannot size"; return false; }
    out.resize((size_t)n);
    size_t got = n ? fread(out.data(), 1, (size_t)n, f) : 0;
    fclose(f);
    if (got != (size_t)n) { err.kind = ErrorKind::Io; err.what = path + ": short read"; return false; }
    return true;
}

} // namespace

bool Parse(const uint8_t* data, size_t size, File& out, Error& err) {
    err = Error{};
    out = File{};
    Reader r{data, size, 0, &err};
    if (!ReadHeader(r, out.header)) return false;
    const Header& h = out.header;
    out.records.reserve((size_t)(h.record_count < 0 ? 0 : (h.record_count > (1 << 16) ? (1 << 16) : h.record_count)));
    for (int32_t i = 0; i < h.record_count; ++i) {
        out.records.emplace_back();
        if (!ReadRecord(r, h.filetype, out.records.back())) return false;
    }
    out.has_trailer = h.filetype == FILETYPE_V15 && r.pos < size;
    if (out.has_trailer) {
        auto list = [&](const char* what, std::vector<int32_t>& v) {
            size_t n; if (!r.Count(4, what, n)) return false;
            v.resize(n);
            for (int32_t& x : v) if (!r.I32(what, x)) return false;
            return true;
        };
        if (!list("zone trailer", out.trailer)) return false;
        while (r.pos < size) { out.trailers.emplace_back(); if (!list("save trailer", out.trailers.back())) return false; }
    }
    if (r.pos != size) return r.Fail(ErrorKind::TrailingBytes, r.pos, "", 0, size - r.pos);
    return true;
}

bool ReadFile(const std::string& path, File& out, Error& err) {
    err = Error{};
    std::vector<uint8_t> bytes;
    if (!ReadBytes(path, bytes, err)) return false;
    return Parse(bytes.data(), bytes.size(), out, err);
}

bool ReadHeader(const std::string& path, Header& out, Error& err) {
    err = Error{};
    out = Header{};
    std::vector<uint8_t> bytes;
    if (!ReadBytes(path, bytes, err)) return false;
    Reader r{bytes.data(), bytes.size(), 0, &err};
    return ReadHeader(r, out);
}

std::vector<std::string> FileList(const std::string& s) {
    std::vector<std::string> out;
    size_t i = 0;
    while (i <= s.size()) {
        size_t j = s.find(',', i);
        if (j == std::string::npos) j = s.size();
        size_t a = i, b = j;
        while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r' || s[a] == '\n')) ++a;
        while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r' || s[b - 1] == '\n')) --b;
        if (b > a) out.emplace_back(s.substr(a, b - a));
        i = j + 1;
    }
    return out;
}

const char* ItemTypeName(int kind) {
    static const char* const N[ITEM_TYPE_COUNT] = {
        "BUILDING", "CHARACTER", "WEAPON", "ARMOUR", "ITEM", "ANIMAL_ANIMATION", "ATTACHMENT", "RACE", "LOCATION", "WAR_SAVESTATE",
        "FACTION", "NULL_ITEM", "ZONE_MAP", "TOWN", "WORLDMAP_CHARACTER", "CHARACTER_APPEARANCE_OLD", "LOCATIONAL_DAMAGE", "COMBAT_TECHNIQUE",
        "DIALOGUE", "DIALOGUE_LINE", "TECHTREE", "RESEARCH", "AI_TASK", "AI_STATE", "ANIMATION", "STATS", "PERSONALITY", "CONSTANTS", "BIOMES",
        "BUILDING_PART", "INSTANCE_COLLECTION", "DIALOG_ACTION", "TEMPORARY_INFO", "MOD_FILENAME", "PLATOON", "GAMESTATE_BUILDING",
        "GAMESTATE_CHARACTER", "GAMESTATE_FACTION", "GAMESTATE_TOWN_INSTANCE_LIST", "STATE", "SAVED_STATE", "INVENTORY_STATE",
        "INVENTORY_ITEM_STATE", "REPEATABLE_BUILDING_PART_SLOT", "MATERIAL_SPEC", "MATERIAL_SPECS_COLLECTION", "CONTAINER",
        "MATERIAL_SPECS_CLOTHING", "GAMESTATE_BUILDING_INTERIOR", "VENDOR_LIST", "MATERIAL_SPECS_WEAPON", "WEAPON_MANUFACTURER",
        "SQUAD_TEMPLATE", "ROAD", "LOCATION_NODE", "COLOR_DATA", "CAMERA", "MEDICAL_STATE", "MEDICAL_PART_STATE", "FOLIAGE_LAYER",
        "FOLIAGE_MESH", "GRASS", "BUILDING_FUNCTIONALITY", "DAY_SCHEDULE", "NEW_GAME_STARTOFF", "GAMESTATE_CRAFTING", "CHARACTER_APPEARANCE",
        "GAMESTATE_AI", "WILDLIFE_BIRDS", "MAP_FEATURES", "DIPLOMATIC_ASSAULTS", "SINGLE_DIPLOMATIC_ASSAULT", "AI_PACKAGE", "DIALOGUE_PACKAGE",
        "GUN_DATA", "HUMAN_CHARACTER", "ANIMAL_CHARACTER", "UNIQUE_SQUAD_TEMPLATE", "FACTION_TEMPLATE", "AI_SCHEDULE", "WEATHER", "SEASON",
        "EFFECT", "ITEM_PLACEMENT_GROUP", "WORD_SWAPS", "NEST", "NEST_ITEM", "CHARACTER_PHYSICS_ATTACHMENT", "LIGHT", "HEAD", "BLUEPRINT",
        "SHOP_TRADER_CLASS", "FOLIAGE_BUILDING", "FACTION_CAMPAIGN", "GAMESTATE_TOWN", "BIOME_GROUP", "EFFECT_FOG_VOLUME", "FARM_DATA",
        "FARM_PART", "ENVIRONMENT_RESOURCES", "RACE_GROUP", "ARTIFACTS", "MAP_ITEM", "BUILDINGS_SWAP", "ITEMS_CULTURE", "ANIMATION_EVENT",
        "TUTORIAL", "CROSSBOW", "TERRAIN_DECALS", "AMBIENT_SOUND", "WORLD_EVENT_STATE", "LIMB_REPLACEMENT", "ANIMATION_FILE", "____XXX___"};
    return (kind >= 0 && kind < ITEM_TYPE_COUNT) ? N[kind] : "?";
}

} // namespace md::fcs

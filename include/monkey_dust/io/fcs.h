// FCS game-data files (`data/*.base`, `data/*.mod`, workshop / `mods/` `.mod`): the binary record format of Kenshi's
// Forgotten Construction Set, versions 15 (zone/level files), 16 and 17. Little-endian, length-prefixed, no alignment.
//
// This is a format reader only: it contains no game data. It is meant for LOAD TIME (files are read once, whole);
// std::string / std::vector are used here on purpose and must not be used from a per-frame path.
//
// Behaviour ported from Project Okran (https://github.com/brayniac6-glitch/Project-Okran, MIT licence):
// src/io/fcs.rs (the format), src/io/mods.rs (load order), src/data/gamedata.rs (record merging).
// Copyright (c) 2026 brayniac6-glitch. See THIRD_PARTY.md for the licence text. Strings are kept as raw bytes
// (the files hold UTF-8 or Latin-1; Okran converts to Rust Strings, here nothing is converted).
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace md::fcs {

constexpr int32_t FILETYPE_V15 = 15;
constexpr int32_t FILETYPE_V16 = 16;
constexpr int32_t FILETYPE_V17 = 17;

// datatype flags of a record (bit 31 is the v16 "changed" flag and is ignored by the merge)
constexpr uint32_t DATATYPE_MODIFIES  = 0x1; // the record modifies an existing one
constexpr uint32_t DATATYPE_SETS_NAME = 0x2; // the record replaces the name of an existing one

struct Header {
    int32_t filetype = 0;     // 15, 16 or 17
    int32_t version = 0;      // v16 / v17
    std::string author, description;
    std::string dependencies; // comma-separated file names this file builds on
    std::string references;   // comma-separated file names referenced but not required
    std::vector<uint8_t> tail; // v17: bytes between `references` and the header end (meaning unknown), kept verbatim
    int32_t id_counter = 0;
    int32_t record_count = 0;
};

struct Reference {
    std::string target;       // string_id of the referenced record
    int32_t values[3] = {0, 0, 0};
};

struct Instance {
    std::string id;
    std::string target;       // string_id of the placed record; empty = remove the instance with this id (merge)
    float position[3] = {0, 0, 0};
    float rotation[4] = {0, 0, 0, 0};
    std::vector<std::string> states;
};

struct Record {
    int32_t lead = 0;         // v16: 0. v17: 0 or the record's byte length including this field
    int32_t kind = 0;         // ItemType
    int32_t id = 0;
    std::string name;
    std::string string_id;
    uint32_t datatype = 0;
    std::vector<std::pair<std::string, bool>> bools;
    std::vector<std::pair<std::string, float>> floats;
    std::vector<std::pair<std::string, int32_t>> ints;
    std::vector<std::pair<std::string, std::array<float, 3>>> vec3s;
    std::vector<std::pair<std::string, std::array<float, 4>>> vec4s;
    std::vector<std::pair<std::string, std::string>> strings;
    std::vector<std::pair<std::string, std::string>> files;
    // (category, references) in file order
    std::vector<std::pair<std::string, std::vector<Reference>>> references;
    std::vector<Instance> instances;
};

struct File {
    Header header;
    std::vector<Record> records;
    // v15 zone files end with an i32 count and that many i32s; empty for other files
    std::vector<int32_t> trailer;
    std::vector<std::vector<int32_t>> trailers;
    bool has_trailer = false;
};

enum class ErrorKind { None, Io, Truncated, UnsupportedFiletype, BadLength, TrailingBytes, LengthMismatch };

struct Error {
    ErrorKind kind = ErrorKind::None;
    size_t at = 0;            // byte offset
    std::string what;         // what was being read / the path (Io)
    int64_t value = 0;        // length / filetype / declared length
    size_t extra = 0;         // bytes left over / bytes parsed
    std::string Message() const;
};

// Parse a whole file. Fails on any leftover bytes, so a successful parse of every shipped file is the check that the
// format is right.
bool Parse(const uint8_t* data, size_t size, File& out, Error& err);
bool ReadFile(const std::string& path, File& out, Error& err);
// Only the header (dependencies, record count): for ordering files without parsing records.
bool ReadHeader(const std::string& path, Header& out, Error& err);

// Split a header's comma-separated file list (`dependencies`, `references`): trimmed, empty entries dropped.
std::vector<std::string> FileList(const std::string& comma_separated);

// ItemType (Record::kind) names, 0..113; "?" for anything else. Source: the FCS editor's itemType enum.
constexpr int ITEM_TYPE_COUNT = 114;
const char* ItemTypeName(int kind);
constexpr int ITEM_BIOMES = 28;

} // namespace md::fcs

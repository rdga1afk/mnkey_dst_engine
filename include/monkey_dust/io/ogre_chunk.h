// Shared reading of Ogre's binary chunk streams (`.mesh`, `.skeleton`): little-endian values, newline-terminated strings,
// `u16 id + u32 length` chunk headers.
//
// This is a format reader only: it contains no game data. LOAD TIME only (std::string / std::vector).
//
// Behaviour ported from Project Okran (https://github.com/brayniac6-glitch/Project-Okran, MIT licence):
// src/io/ogre.rs. Copyright (c) 2026 brayniac6-glitch. See THIRD_PARTY.md for the licence text.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace md::ogre {

enum class ErrorKind { None, Truncated, BadHeader, BadChunk, Invalid, Io };

struct Error {
    ErrorKind kind = ErrorKind::None;
    size_t at = 0;        // byte offset
    std::string what;
    uint32_t id = 0, len = 0;  // BadChunk / BadHeader
    std::string Message() const;
};

struct Chunk {
    uint16_t id = 0;
    size_t start = 0;
    uint32_t len = 0;
};

// Reads a byte buffer. After the first failure `Failed()` stays true and every read returns zero / empty; the error says what was being read.
class Reader {
public:
    Reader(const uint8_t* data, size_t size) : buf_(data), size_(size) {}
    size_t Pos() const { return pos_; }
    size_t Size() const { return size_; }
    bool AtEnd() const { return pos_ >= size_; }
    bool Failed() const { return err_.kind != ErrorKind::None; }
    const Error& Err() const { return err_; }
    void Fail(ErrorKind kind, size_t at, std::string what);

    uint8_t U8(const char* what);
    uint16_t U16(const char* what);
    uint32_t U32(const char* what);
    float F32(const char* what);
    void F32x3(const char* what, float out[3]);
    // `\n`-terminated string (the terminator is consumed, not returned).
    std::string Line(const char* what);
    const uint8_t* Take(size_t n, const char* what);

    // Chunk header. The stored length is trusted only by Skip: older serializers (v1.8, v1.41) write wrong lengths for parent chunks,
    // so known chunks are read by content.
    Chunk Header();
    // Jump past a chunk that is not decoded, by its stored length.
    void Skip(const Chunk& c);
    // The next child chunk if its id is one the parent can contain; otherwise false, consuming nothing (how Ogre's own reader finds the end of a parent).
    bool Child(const uint16_t* allowed, size_t n, Chunk& out);
    bool Child(uint16_t only, Chunk& out) { return Child(&only, 1, out); }

private:
    const uint8_t* buf_;
    size_t size_, pos_ = 0;
    Error err_;
};

} // namespace md::ogre

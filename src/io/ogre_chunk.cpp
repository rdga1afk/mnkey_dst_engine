// Behaviour ported from Project Okran src/io/ogre.rs (MIT, Copyright (c) 2026 brayniac6-glitch). See THIRD_PARTY.md.
#include <monkey_dust/io/ogre_chunk.h>
#include <cstring>

namespace md::ogre {

std::string Error::Message() const {
    switch (kind) {
    case ErrorKind::Truncated: return "truncated reading " + what + " at byte " + std::to_string(at);
    case ErrorKind::BadHeader: return "not an Ogre file (header id " + std::to_string(id) + ")";
    case ErrorKind::BadChunk: return "bad chunk " + std::to_string(id) + " length " + std::to_string(len) + " at byte " + std::to_string(at);
    case ErrorKind::Invalid: return what + " (chunk at byte " + std::to_string(at) + ")";
    case ErrorKind::Io: return what;
    default: return "";
    }
}

void Reader::Fail(ErrorKind kind, size_t at, std::string what) {
    if (Failed()) return;
    err_.kind = kind; err_.at = at; err_.what = std::move(what);
}

const uint8_t* Reader::Take(size_t n, const char* what) {
    if (Failed()) return nullptr;
    if (n > size_ - pos_) { Fail(ErrorKind::Truncated, pos_, what); return nullptr; }
    const uint8_t* p = buf_ + pos_;
    pos_ += n;
    return p;
}

uint8_t Reader::U8(const char* what) { const uint8_t* p = Take(1, what); return p ? p[0] : 0; }
uint16_t Reader::U16(const char* what) { const uint8_t* p = Take(2, what); return p ? uint16_t(p[0] | (p[1] << 8)) : 0; }
uint32_t Reader::U32(const char* what) {
    const uint8_t* p = Take(4, what);
    return p ? uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24 : 0;
}
float Reader::F32(const char* what) { uint32_t u = U32(what); float f; std::memcpy(&f, &u, 4); return f; }
void Reader::F32x3(const char* what, float out[3]) { for (int i = 0; i < 3; ++i) out[i] = F32(what); }

std::string Reader::Line(const char* what) {
    if (Failed()) return {};
    const void* nl = pos_ < size_ ? std::memchr(buf_ + pos_, '\n', size_ - pos_) : nullptr;
    if (!nl) { Fail(ErrorKind::Truncated, pos_, what); return {}; }
    size_t n = size_t(static_cast<const uint8_t*>(nl) - (buf_ + pos_));
    std::string s(reinterpret_cast<const char*>(buf_ + pos_), n);
    pos_ += n + 1;
    return s;
}

Chunk Reader::Header() {
    Chunk c; c.start = pos_;
    c.id = U16("chunk id"); c.len = U32("chunk length");
    return c;
}

void Reader::Skip(const Chunk& c) {
    if (Failed()) return;
    size_t end = c.start + size_t(c.len);
    if (c.len < 6 || end > size_) { Fail(ErrorKind::BadChunk, c.start, ""); err_.id = c.id; err_.len = c.len; return; }
    pos_ = end;
}

bool Reader::Child(const uint16_t* allowed, size_t n, Chunk& out) {
    if (Failed() || size_ - pos_ < 6) return false;
    uint16_t id = uint16_t(buf_[pos_] | (buf_[pos_ + 1] << 8));
    bool ok = false;
    for (size_t i = 0; i < n; ++i) ok = ok || allowed[i] == id;
    if (!ok) return false;
    out = Header();
    return !Failed();
}

} // namespace md::ogre

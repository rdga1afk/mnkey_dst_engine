#include "hkt.h"
#include <cstring>

namespace md::hkt {
namespace {
constexpr int64_t kVoid = 0, kByte = 1, kInt = 2, kReal = 3, kVec4 = 4, kVec16 = 7, kObject = 8, kStruct = 9, kCString = 10, kArray = 0x10, kTuple = 0x20;
struct Type { int64_t kind = 0; size_t tuple = 0; std::string cls; };
struct Class { std::string name; size_t parent = 0; std::vector<std::pair<std::string, Type>> members; };
struct Fail {};

struct Reader {
    const std::vector<uint8_t>& b; size_t at = 8; std::string err;
    std::vector<std::pair<bool, std::string>> strings{ {true, ""}, {false, ""} };   // (present, text); entry 1 is null
    std::vector<Class> classes;
    explicit Reader(const std::vector<uint8_t>& bytes) : b(bytes) {}
    [[noreturn]] void Throw(const std::string& m) { err = m + " at " + std::to_string(at); throw Fail{}; }
    uint8_t Byte() { if (at >= b.size()) Throw("truncated"); return b[at++]; }
    const uint8_t* Take(size_t n) { if (at + n > b.size()) Throw("truncated"); const uint8_t* p = &b[at]; at += n; return p; }
    int64_t Int() { uint64_t v = 0; int sh = 0; for (;;) { uint8_t c = Byte(); v |= (uint64_t)(c & 0x7f) << sh; sh += 7; if (!(c & 0x80)) break; if (sh > 63) Throw("int too long"); } int64_t m = (int64_t)(v >> 1); return (v & 1) ? -m : m; }
    float F32() { float f; std::memcpy(&f, Take(4), 4); return f; }
    std::string Str() {   // "" for null
        int64_t n = Int();
        if (n > 0) { std::string s((const char*)Take((size_t)n), (size_t)n); strings.push_back({true, s}); return s; }
        size_t i = (size_t)(-n); if (i >= strings.size()) Throw("string index"); return strings[i].second;
    }
    size_t ClassIndex() { int64_t i = Int(); if (i <= 0 || (size_t)i > classes.size()) Throw("class index"); return (size_t)i; }
    void ReadClass() {
        Class c; c.name = Str(); Int(); c.parent = (size_t)Int(); int64_t n = Int();
        for (int64_t k = 0; k < n; ++k) {
            std::string m = Str(); Type t; t.kind = Int();
            if (t.kind & kTuple) t.tuple = (size_t)Int();
            int64_t base = t.kind & 0xf; if (base == kObject || base == kStruct) t.cls = Str();
            c.members.push_back({m, t});
        }
        classes.push_back(std::move(c));
    }
    std::vector<std::pair<std::string, Type>> Members(size_t ci) {
        const Class& c = classes[ci - 1]; std::vector<std::pair<std::string, Type>> out;
        if (c.parent) out = Members(c.parent);
        out.insert(out.end(), c.members.begin(), c.members.end()); return out;
    }
    std::vector<bool> Bits(size_t n) { const uint8_t* p = Take((n + 7) / 8); std::vector<bool> r(n); for (size_t i = 0; i < n; ++i) r[i] = (p[i / 8] >> (i % 8)) & 1; return r; }
    size_t StructClass(const Type& t) { for (size_t i = 0; i < classes.size(); ++i) if (classes[i].name == t.cls) return i + 1; Throw("undeclared class " + t.cls); }
    Object Body(size_t ci) {
        auto members = Members(ci); auto present = Bits(members.size()); Object o; o.cls = classes[ci - 1].name;
        for (size_t k = 0; k < members.size(); ++k) if (present[k]) o.fields.push_back({members[k].first, ValueOf(members[k].second)});
        return o;
    }
    Value ValueOf(const Type& ty) {
        Type base = ty; base.kind &= 0xf;
        if (ty.kind & kTuple) return ArrayOf(base, ty.tuple);
        if (ty.kind & kArray) return ArrayOf(base, (size_t)Int());
        return Scalar(base);
    }
    Value Scalar(const Type& ty) {
        Value v;
        switch (ty.kind) {
            case kByte: v.kind = Value::Byte; v.i = Byte(); break;
            case kInt: v.kind = Value::Int; v.i = Int(); break;
            case kReal: v.kind = Value::Real; v.r = F32(); break;
            case kCString: v.kind = Value::Str; v.s = Str(); break;
            case kObject: v.kind = Value::Ref; v.i = Int(); break;
            case kStruct: v.kind = Value::Struct; v.st.push_back(Body(StructClass(ty))); break;
            default:
                if (ty.kind >= kVec4 && ty.kind <= kVec16) { v.kind = Value::Vec; size_t n = 4 * (size_t)(ty.kind - kVec4 + 1); for (size_t k = 0; k < n; ++k) v.v.push_back(F32()); break; }
                Throw("value type " + std::to_string(ty.kind));
        }
        return v;
    }
    void Width() { int64_t w = Int(); if (w != 4) Throw("array width " + std::to_string(w)); }
    Value ArrayOf(const Type& ty, size_t n) {
        Value out;
        if (ty.kind == kByte) { out.kind = Value::Bytes; const uint8_t* p = Take(n); out.bytes.assign(p, p + n); return out; }
        out.kind = Value::Array;
        if (ty.kind == kInt) { Width(); for (size_t k = 0; k < n; ++k) out.a.push_back(Scalar(ty)); return out; }
        if (ty.kind == kVec4) {
            int64_t stored = Int(); if (stored < 1 || stored > 4) Throw("vec4 width");
            for (size_t k = 0; k < n; ++k) { Value e; e.kind = Value::Vec; e.v.assign(4, 0.f); for (int64_t j = 0; j < stored; ++j) e.v[(size_t)j] = F32(); out.a.push_back(std::move(e)); }
            return out;
        }
        if (ty.kind == kStruct) {
            size_t ci = StructClass(ty); auto members = Members(ci); auto present = Bits(members.size());
            std::vector<Object> objs(n); for (auto& o : objs) o.cls = classes[ci - 1].name;
            for (size_t m = 0; m < members.size(); ++m) {
                if (!present[m]) continue; const Type& mt = members[m].second;
                for (size_t k = 0; k < n; ++k) objs[k].fields.push_back({members[m].first, Value{}});
                if (mt.kind & (kArray | kTuple)) { for (size_t k = 0; k < n; ++k) objs[k].fields.back().second = ValueOf(mt); }
                else {
                    Value col = ArrayOf(mt, n);
                    for (size_t k = 0; k < n; ++k) {
                        Value e;
                        if (col.kind == Value::Bytes) { e.kind = Value::Byte; e.i = col.bytes[k]; } else e = std::move(col.a[k]);
                        objs[k].fields.back().second = std::move(e);
                    }
                }
            }
            for (size_t k = 0; k < n; ++k) { Value e; e.kind = Value::Struct; e.st.push_back(std::move(objs[k])); out.a.push_back(std::move(e)); }
            return out;
        }
        if (ty.kind == kReal || ty.kind == kObject || ty.kind == kCString) { for (size_t k = 0; k < n; ++k) out.a.push_back(Scalar(ty)); return out; }
        Throw("array of type " + std::to_string(ty.kind));
    }
};
}  // namespace

bool Parse(const std::vector<uint8_t>& bytes, Tagfile& out, std::string& err) {
    static const uint8_t magic[8] = {0x1e, 0x0d, 0xb0, 0xca, 0xce, 0xfa, 0x11, 0xd0};
    if (bytes.size() < 8 || std::memcmp(bytes.data(), magic, 8) != 0) { err = "not a Havok binary tagfile"; return false; }
    Reader r(bytes);
    try {
        for (;;) {
            switch (r.Int()) {
                case 1: r.Int(); r.Str(); r.Take(6); break;
                case 2: r.ReadClass(); break;
                case 4: { size_t ci = r.ClassIndex(); out.objects.push_back(r.Body(ci)); break; }
                case 7: return true;
                default: r.Throw("unsupported tag");
            }
        }
    } catch (const Fail&) { err = r.err; return false; }
}

}  // namespace md::hkt

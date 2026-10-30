// Havok binary tagfile (.hkt, hk_2014.2.0-r1, tagfile v5) reader: the format Kenshi ships its navmesh tiles in
// (data/newland/land/navtiles/tileX.Y.hkt). Layout from the shipped files (8-byte magic, then 7-bit variable-length ints with the sign in bit 0):
// tag 1 file info, 2 class (name, version, parent, members), 4 object (class index, a presence bit per member parents-first, then the present
// values), 7 end. Strings go through a table that starts as ["", null]. Arrays of structs are stored member by member (a presence bit, then all
// elements' values). Not the vendored hkxparse: that one does not resolve the object references of the root container.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace md::hkt {

struct Object;
struct Value {
    enum Kind { None, Byte, Int, Real, Vec, Str, Ref, Struct, Array, Bytes } kind = None;
    int64_t i = 0;                // Byte, Int, Ref (object number, 0 = none)
    float r = 0;                  // Real
    std::vector<float> v;         // Vec
    std::string s;                // Str
    std::vector<Value> a;         // Array
    std::vector<uint8_t> bytes;   // Bytes
    std::vector<Object> st;       // Struct (one element)
};
struct Object {
    std::string cls;
    std::vector<std::pair<std::string, Value>> fields;
    const Value* Get(const std::string& n) const { for (auto& f : fields) if (f.first == n) return &f.second; return nullptr; }
    int64_t Int(const std::string& n) const { const Value* v = Get(n); return v && (v->kind == Value::Int || v->kind == Value::Byte || v->kind == Value::Ref) ? v->i : 0; }
    float Real(const std::string& n) const { const Value* v = Get(n); return v && v->kind == Value::Real ? v->r : 0.f; }
    const std::vector<Value>& Array(const std::string& n) const { static const std::vector<Value> none; const Value* v = Get(n); return v && v->kind == Value::Array ? v->a : none; }
};
struct Tagfile {
    std::vector<Object> objects;   // object number n is objects[n - 1]
    const Object* At(int64_t number) const { return number >= 1 && (size_t)number <= objects.size() ? &objects[(size_t)number - 1] : nullptr; }
};
// false + err on a malformed / unsupported file
bool Parse(const std::vector<uint8_t>& bytes, Tagfile& out, std::string& err);

}  // namespace md::hkt

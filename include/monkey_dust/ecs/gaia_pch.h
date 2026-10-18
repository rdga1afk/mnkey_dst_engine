#pragma once
// Precompiled header for the targets that use gaia-ecs (see cmake/MdGaiaPch.cmake, #187).
// Built with -fpch-codegen -fpch-instantiate-templates: the gaia code instantiated below (query machinery, the sort networks, World setup)
// is compiled ONCE into the target's PCH object instead of in every translation unit that touches a query (one query = 30-40 s of backend
// time per TU before this). The function is never called; it only has to instantiate the component-independent parts of the library.
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <cmath>
#include <monkey_dust/ecs/md_registry.h>

namespace md_gaia_pch {
struct CompA { float x; };
struct CompB { float y; };
struct CompC { int z; };

inline void Warm(gaia::ecs::World& w) {
    auto e = w.add();
    w.add<CompA>(e, {1.f});
    w.add<CompB>(e, {2.f});
    w.add<CompC>(e, {3});
    float s = 0.f;
    w.query().all<CompA>().all<CompB>().each([&](const CompA& a, const CompB& b) { s += a.x + b.y; });
    w.query().all<CompA&>().no<CompC>().each([&](CompA& a) { a.x += s; });
    w.query().any<CompB>().all<CompA>().each([&](gaia::ecs::Iter& it) { s += (float)it.size(); });
    s += (float)w.query().all<CompC>().count();
    if (w.has<CompA>(e)) s += w.get<CompA>(e).x;
    w.set<CompB>(e) = {s};
    w.del<CompC>(e);
    w.del(e);
}
} // namespace md_gaia_pch

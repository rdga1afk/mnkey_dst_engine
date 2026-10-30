#include <monkey_dust/nav/navtile.h>
#include "hkt.h"
#include <DetourNavMesh.h>
#include <DetourNavMeshBuilder.h>
#include <DetourAlloc.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>

namespace md::navtile {

bool LoadTile(const std::string& dir, int x, int y, std::vector<Section>& out, std::string& err) {
    char name[96]; std::snprintf(name, sizeof name, "/data/newland/land/navtiles/tile%d.%d.hkt", x, y);
    std::ifstream in(dir + name, std::ios::binary);
    if (!in) return true;   // the world has no file for this tile
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    hkt::Tagfile tf; if (!hkt::Parse(bytes, tf, err)) return false;
    const hkt::Object* root = tf.At(1); if (!root) { err = "navtile without root"; return false; }
    struct Group { int64_t mesh = 0, info = 0; };
    std::vector<Group> groups;
    for (const hkt::Value& nv : root->Array("namedVariants")) {
        if (nv.kind != hkt::Value::Struct) continue;
        const hkt::Object& v = nv.st[0]; const hkt::Value* nm = v.Get("name");
        if (!nm || nm->kind != hkt::Value::Str) continue;
        if (nm->s == "NavMesh") { groups.push_back({}); groups.back().mesh = v.Int("variant"); }
        else if (nm->s == "Info" && !groups.empty()) groups.back().info = v.Int("variant");
    }
    for (const Group& g : groups) {
        const hkt::Object* mesh = tf.At(g.mesh); if (!mesh) { err = "navtile without hkaiNavMesh"; return false; }
        Section s; s.tile_x = x; s.tile_y = y;
        if (const hkt::Object* io = tf.At(g.info)) { const hkt::Value* sv = io->Get("string"); if (sv && sv->kind == hkt::Value::Str) s.info = sv->s; s.building = !s.info.empty(); }
        float o[3] = { x * kZoneMetres, 0.f, y * kZoneMetres };
        if (s.building) {   // "uid hex;x;y;z" world units, centred world: engine corner coordinates add the half world
            float f[3]; int n = 0; size_t p = s.info.find(';');
            while (p != std::string::npos && n < 3) { f[n++] = (float)std::atof(s.info.c_str() + p + 1); p = s.info.find(';', p + 1); }
            if (n != 3) { err = "navtile building position"; return false; }
            o[0] = f[0] * 0.1f + kWorldMetres * 0.5f; o[1] = f[1] * 0.1f; o[2] = f[2] * 0.1f + kWorldMetres * 0.5f;
        }
        for (const hkt::Value& v : mesh->Array("vertices")) {
            if (v.kind != hkt::Value::Vec || v.v.size() < 3) { err = "navtile vertex"; return false; }
            s.vertices.push_back(o[0] + v.v[0]); s.vertices.push_back(o[1] + v.v[1]); s.vertices.push_back(o[2] + v.v[2]);
        }
        int64_t stride = std::max<int64_t>(0, mesh->Int("faceDataStriding")); const auto& fd = mesh->Array("faceData");
        const auto& fa = mesh->Array("faces");
        for (size_t i = 0; i < fa.size(); ++i) {
            if (fa[i].kind != hkt::Value::Struct) { err = "navtile face"; return false; }
            const hkt::Object& f = fa[i].st[0]; Face face;
            face.start_edge = (uint32_t)f.Int("startEdgeIndex"); face.num_edges = (uint32_t)f.Int("numEdges"); face.cluster = (int32_t)f.Int("clusterIndex");
            face.kind = (stride > 0 && i * (size_t)stride < fd.size()) ? (int32_t)fd[i * (size_t)stride].i : -1;
            s.faces.push_back(face);
        }
        for (const hkt::Value& e : mesh->Array("edges")) {
            if (e.kind != hkt::Value::Struct) { err = "navtile edge"; return false; }
            const hkt::Object& eo = e.st[0]; Edge ed;
            ed.a = (uint32_t)eo.Int("a"); ed.b = (uint32_t)eo.Int("b"); ed.opposite_face = (int32_t)eo.Int("oppositeFace"); ed.opposite_edge = (int32_t)eo.Int("oppositeEdge");
            s.edges.push_back(ed);
        }
        out.push_back(std::move(s));
    }
    return true;
}

namespace {
struct Tri { uint32_t v[3]; int area; };
// Ear clipping of a simple polygon in the XZ plane (either winding); falls back to a fan when no ear is found (degenerate loops).
void Triangulate(const std::vector<uint32_t>& loop, const std::vector<float>& vx, int area, std::vector<Tri>& out) {
    auto X = [&](uint32_t i) { return vx[i * 3]; }; auto Z = [&](uint32_t i) { return vx[i * 3 + 2]; };
    double sum = 0; for (size_t i = 0; i < loop.size(); ++i) { uint32_t a = loop[i], b = loop[(i + 1) % loop.size()]; sum += (double)X(a) * Z(b) - (double)X(b) * Z(a); }
    const double sgn = sum >= 0 ? 1.0 : -1.0;
    std::vector<uint32_t> p = loop;
    auto cross = [&](uint32_t a, uint32_t b, uint32_t c) { return sgn * (((double)X(b) - X(a)) * ((double)Z(c) - Z(a)) - ((double)Z(b) - Z(a)) * ((double)X(c) - X(a))); };
    while (p.size() > 3) {
        bool clipped = false;
        for (size_t i = 0; i < p.size(); ++i) {
            uint32_t a = p[(i + p.size() - 1) % p.size()], b = p[i], c = p[(i + 1) % p.size()];
            if (cross(a, b, c) <= 1e-9) continue;
            bool inside = false;
            for (uint32_t q : p) { if (q == a || q == b || q == c) continue; if (cross(a, b, q) >= 0 && cross(b, c, q) >= 0 && cross(c, a, q) >= 0) { inside = true; break; } }
            if (inside) continue;
            out.push_back({{a, b, c}, area}); p.erase(p.begin() + (long)i); clipped = true; break;
        }
        if (!clipped) { for (size_t i = 1; i + 1 < p.size(); ++i) out.push_back({{p[0], p[i], p[i + 1]}, area}); return; }
    }
    if (p.size() == 3) out.push_back({{p[0], p[1], p[2]}, area});
}
}  // namespace

dtNavMesh* BuildDetour(const Section& s, std::string& err) {
    std::vector<Tri> tris;
    for (const Face& f : s.faces) {
        if (f.kind == 3 || f.kind == 5 || f.num_edges < 3) continue;
        std::vector<uint32_t> loop;
        for (uint32_t k = 0; k < f.num_edges; ++k) { size_t ei = (size_t)f.start_edge + k; if (ei >= s.edges.size()) { err = "edge index"; return nullptr; } loop.push_back(s.edges[ei].a); }
        Triangulate(loop, s.vertices, f.kind == 4 ? 2 : 1, tris);
    }
    if (tris.empty()) { err = "no walkable faces"; return nullptr; }
    float bmin[3] = {1e30f, 1e30f, 1e30f}, bmax[3] = {-1e30f, -1e30f, -1e30f};
    for (const Tri& t : tris) for (int k = 0; k < 3; ++k) for (int d = 0; d < 3; ++d) { float c = s.vertices[t.v[k] * 3 + d]; bmin[d] = std::min(bmin[d], c); bmax[d] = std::max(bmax[d], c); }
    const float cs = 0.05f, ch = 0.02f;
    if ((bmax[0] - bmin[0]) / cs > 65000.f || (bmax[2] - bmin[2]) / cs > 65000.f || (bmax[1] - bmin[1]) / ch > 65000.f) { err = "section too large for 16-bit cells"; return nullptr; }
    // used vertices -> compact indices, quantised
    std::map<uint32_t, uint16_t> remap; std::vector<uint16_t> verts;
    auto vid = [&](uint32_t v) { auto it = remap.find(v); if (it != remap.end()) return it->second; uint16_t id = (uint16_t)remap.size(); remap[v] = id;
        verts.push_back((uint16_t)std::lround((s.vertices[v * 3] - bmin[0]) / cs)); verts.push_back((uint16_t)std::lround((s.vertices[v * 3 + 1] - bmin[1]) / ch)); verts.push_back((uint16_t)std::lround((s.vertices[v * 3 + 2] - bmin[2]) / cs)); return id; };
    const int nvp = 6; const size_t np = tris.size();
    std::vector<uint16_t> polys(np * nvp * 2, 0xffff); std::vector<unsigned char> areas(np); std::vector<unsigned short> flags(np, 1);
    std::map<std::pair<uint32_t, uint32_t>, std::pair<uint32_t, int>> edge_owner;   // directed (a, b) -> (poly, edge)
    for (size_t i = 0; i < np; ++i) {
        const Tri& t = tris[i]; areas[i] = (unsigned char)t.area;
        // Detour wants clockwise winding seen from above (x right, z down): make the triangle clockwise in (x, z)
        uint32_t a = t.v[0], b = t.v[1], c = t.v[2];
        double cr = ((double)s.vertices[b * 3] - s.vertices[a * 3]) * ((double)s.vertices[c * 3 + 2] - s.vertices[a * 3 + 2]) - ((double)s.vertices[b * 3 + 2] - s.vertices[a * 3 + 2]) * ((double)s.vertices[c * 3] - s.vertices[a * 3]);
        if (cr > 0) std::swap(b, c);
        uint32_t idx[3] = {a, b, c};
        for (int k = 0; k < 3; ++k) polys[i * nvp * 2 + k] = vid(idx[k]);
        for (int k = 0; k < 3; ++k) edge_owner[{idx[k], idx[(k + 1) % 3]}] = {(uint32_t)i, k};
    }
    for (auto& e : edge_owner) { auto o = edge_owner.find({e.first.second, e.first.first}); if (o != edge_owner.end()) polys[e.second.first * nvp * 2 + nvp + (size_t)e.second.second] = (uint16_t)o->second.first; }
    dtNavMeshCreateParams p; std::memset(&p, 0, sizeof p);
    p.verts = verts.data(); p.vertCount = (int)(verts.size() / 3); p.polys = polys.data(); p.polyAreas = areas.data(); p.polyFlags = flags.data(); p.polyCount = (int)np; p.nvp = nvp;
    p.walkableHeight = 1.8f; p.walkableRadius = 0.4f; p.walkableClimb = 0.6f; std::memcpy(p.bmin, bmin, sizeof bmin); std::memcpy(p.bmax, bmax, sizeof bmax); p.cs = cs; p.ch = ch; p.buildBvTree = true;
    unsigned char* data = nullptr; int dsz = 0;
    if (!dtCreateNavMeshData(&p, &data, &dsz)) { err = "dtCreateNavMeshData failed"; return nullptr; }
    dtNavMesh* nav = dtAllocNavMesh(); dtNavMeshParams np2; std::memset(&np2, 0, sizeof np2); std::memcpy(np2.orig, bmin, sizeof bmin);
    np2.tileWidth = bmax[0] - bmin[0] + 1.f; np2.tileHeight = bmax[2] - bmin[2] + 1.f; np2.maxTiles = 1; np2.maxPolys = (int)np;
    if (dtStatusFailed(nav->init(&np2)) || dtStatusFailed(nav->addTile(data, dsz, DT_TILE_FREE_DATA, 0, nullptr))) { dtFreeNavMesh(nav); dtFree(data); err = "navmesh init / addTile failed"; return nullptr; }
    return nav;
}

namespace {
struct Poly { std::vector<uint32_t> v; int area; };   // vertex loop of a section, clockwise in (x, z) seen from above (Detour's winding)
double Cross2(const std::vector<float>& vx, uint32_t a, uint32_t b, uint32_t c) {
    return ((double)vx[b * 3] - vx[a * 3]) * ((double)vx[c * 3 + 2] - vx[a * 3 + 2]) - ((double)vx[b * 3 + 2] - vx[a * 3 + 2]) * ((double)vx[c * 3] - vx[a * 3]);
}
void MakeClockwise(const std::vector<float>& vx, std::vector<uint32_t>& l) {   // the same test as BuildDetour: Cross2 > 0 = counter-clockwise in this convention
    double sum = 0; for (size_t i = 0; i < l.size(); ++i) { uint32_t a = l[i], b = l[(i + 1) % l.size()]; sum += (double)vx[a * 3] * vx[b * 3 + 2] - (double)vx[b * 3] * vx[a * 3 + 2]; }
    if (sum > 0) std::reverse(l.begin(), l.end());   // positive shoelace sum in (x, z) = the winding BuildDetour swaps away
}
bool ConvexCw(const std::vector<float>& vx, const std::vector<uint32_t>& l) {
    for (size_t i = 0; i < l.size(); ++i) if (Cross2(vx, l[i], l[(i + 1) % l.size()], l[(i + 2) % l.size()]) > 1e-9) return false;
    return true;
}
// The walkable faces of a section as convex polygons of at most 6 vertices: a convex face stays one polygon; the others are ear clipped and the triangles merged back (greedy, longest shared edge first).
void SectionPolys(const Section& s, std::vector<Poly>& out, PolyStats* st) {
    for (const Face& f : s.faces) {
        if (f.kind == 3 || f.kind == 5 || f.num_edges < 3) continue;
        std::vector<uint32_t> loop;
        for (uint32_t k = 0; k < f.num_edges; ++k) { size_t ei = (size_t)f.start_edge + k; if (ei >= s.edges.size()) return; loop.push_back(s.edges[ei].a); }
        const int area = f.kind == 4 ? 2 : 1;
        double sum = 0; for (size_t i = 0; i < loop.size(); ++i) { uint32_t a = loop[i], b = loop[(i + 1) % loop.size()]; sum += (double)s.vertices[a * 3] * s.vertices[b * 3 + 2] - (double)s.vertices[b * 3] * s.vertices[a * 3 + 2]; }
        if (std::fabs(sum) < 1e-6) { if (st) ++st->dropped; continue; }
        if (st) ++st->faces;
        std::vector<uint32_t> cw = loop; MakeClockwise(s.vertices, cw);
        if (cw.size() <= 6 && ConvexCw(s.vertices, cw)) { out.push_back({cw, area}); if (st) ++st->kept; continue; }
        std::vector<Tri> tris; Triangulate(loop, s.vertices, area, tris);
        std::vector<Poly> ps;
        for (const Tri& t : tris) { std::vector<uint32_t> l = {t.v[0], t.v[1], t.v[2]}; if (std::fabs(Cross2(s.vertices, l[0], l[1], l[2])) < 1e-12) continue; MakeClockwise(s.vertices, l); ps.push_back({l, area}); }
        if (st) { ++st->split; st->triangles += (long)ps.size(); }
        for (;;) {
            int bi = -1, bj = -1, bea = 0, beb = 0; double bl = -1; std::vector<uint32_t> bm;
            for (size_t i = 0; i < ps.size(); ++i) for (size_t j = i + 1; j < ps.size(); ++j) {
                if (ps[i].v.size() + ps[j].v.size() - 2 > 6) continue;
                const auto& pa = ps[i].v; const auto& pb = ps[j].v; const size_t na = pa.size(), nb = pb.size();
                for (size_t ea = 0; ea < na; ++ea) for (size_t eb = 0; eb < nb; ++eb) {
                    if (pa[ea] != pb[(eb + 1) % nb] || pa[(ea + 1) % na] != pb[eb]) continue;
                    std::vector<uint32_t> m;
                    for (size_t k = 0; k < na - 1; ++k) m.push_back(pa[(ea + 1 + k) % na]);
                    for (size_t k = 0; k < nb - 1; ++k) m.push_back(pb[(eb + 1 + k) % nb]);
                    if (!ConvexCw(s.vertices, m)) continue;
                    const double dx = (double)s.vertices[pa[ea] * 3] - s.vertices[pa[(ea + 1) % na] * 3], dz = (double)s.vertices[pa[ea] * 3 + 2] - s.vertices[pa[(ea + 1) % na] * 3 + 2];
                    const double len = dx * dx + dz * dz;
                    if (len > bl) { bl = len; bi = (int)i; bj = (int)j; bm = std::move(m); }
                }
            }
            if (bi < 0) break;
            ps[(size_t)bi].v = bm; ps.erase(ps.begin() + bj);
        }
        for (Poly& q : ps) out.push_back(std::move(q));
    }
    if (st) st->polys += (long)out.size();
}
}  // namespace

bool BuildTileData(const Section& s, int tx, int ty, unsigned char** data_out, int* size, int* npolys, PolyStats* stats, std::string& err) {
    const float cs = 0.05f, ch = 0.02f, W = kZoneMetres;
    if (npolys) *npolys = 0;
    std::vector<Poly> polys; SectionPolys(s, polys, stats);
    if (polys.empty()) { *size = 0; *data_out = nullptr; return true; }
    float ymin = 1e30f, ymax = -1e30f; for (const Poly& q : polys) for (uint32_t v : q.v) { ymin = std::min(ymin, s.vertices[v * 3 + 1]); ymax = std::max(ymax, s.vertices[v * 3 + 1]); }
    if ((ymax - ymin) / ch > 65000.f) { err = "tile too high for 16-bit cells"; return false; }
    const float bmin[3] = {tx * W, ymin, ty * W}, bmax[3] = {tx * W + W, ymax, ty * W + W};
    std::map<uint32_t, uint16_t> remap; std::vector<uint16_t> verts;
    auto vid = [&](uint32_t v) { auto it = remap.find(v); if (it != remap.end()) return it->second; uint16_t id = (uint16_t)remap.size(); remap[v] = id;
        verts.push_back((uint16_t)std::lround((s.vertices[v * 3] - bmin[0]) / cs)); verts.push_back((uint16_t)std::lround((s.vertices[v * 3 + 1] - bmin[1]) / ch)); verts.push_back((uint16_t)std::lround((s.vertices[v * 3 + 2] - bmin[2]) / cs)); return id; };
    const int nvp = 6; const size_t np = polys.size();
    std::vector<uint16_t> pv(np * nvp * 2, 0xffff); std::vector<unsigned char> areas(np); std::vector<unsigned short> flags(np, 1);
    std::map<std::pair<uint32_t, uint32_t>, std::pair<uint32_t, int>> owner;   // directed vertex pair -> (poly, edge)
    for (size_t i = 0; i < np; ++i) {
        areas[i] = (unsigned char)polys[i].area;
        for (size_t k = 0; k < polys[i].v.size(); ++k) { pv[i * nvp * 2 + k] = vid(polys[i].v[k]); owner[{polys[i].v[k], polys[i].v[(k + 1) % polys[i].v.size()]}] = {(uint32_t)i, (int)k}; }
    }
    long portals = 0;
    for (auto& e : owner) {
        auto o = owner.find({e.first.second, e.first.first});
        uint16_t& nei = pv[e.second.first * nvp * 2 + nvp + (size_t)e.second.second];
        if (o != owner.end()) { nei = (uint16_t)o->second.first; continue; }
        const float ax = s.vertices[e.first.first * 3], az = s.vertices[e.first.first * 3 + 2], bx = s.vertices[e.first.second * 3], bz = s.vertices[e.first.second * 3 + 2];
        const float tol = 0.06f;   // a border edge lies on the tile line: Recast's portal codes 0 = -x, 1 = +z, 2 = +x, 3 = -z
        if (std::fabs(ax - bmin[0]) < tol && std::fabs(bx - bmin[0]) < tol) nei = 0x8000 | 0;
        else if (std::fabs(az - bmax[2]) < tol && std::fabs(bz - bmax[2]) < tol) nei = 0x8000 | 1;
        else if (std::fabs(ax - bmax[0]) < tol && std::fabs(bx - bmax[0]) < tol) nei = 0x8000 | 2;
        else if (std::fabs(az - bmin[2]) < tol && std::fabs(bz - bmin[2]) < tol) nei = 0x8000 | 3;
        if (nei & 0x8000) ++portals;
    }
    if (stats) stats->portal_edges += portals;
    dtNavMeshCreateParams p; std::memset(&p, 0, sizeof p);
    p.verts = verts.data(); p.vertCount = (int)(verts.size() / 3); p.polys = pv.data(); p.polyAreas = areas.data(); p.polyFlags = flags.data(); p.polyCount = (int)np; p.nvp = nvp;
    p.walkableHeight = 1.8f; p.walkableRadius = 0.4f; p.walkableClimb = 0.6f; std::memcpy(p.bmin, bmin, sizeof bmin); std::memcpy(p.bmax, bmax, sizeof bmax); p.cs = cs; p.ch = ch; p.buildBvTree = true; p.tileX = tx; p.tileY = ty;
    unsigned char* data = nullptr; int dsz = 0;
    if (!dtCreateNavMeshData(&p, &data, &dsz)) { err = "dtCreateNavMeshData failed for tile " + std::to_string(tx) + "." + std::to_string(ty); return false; }
    *data_out = data; *size = dsz; if (npolys) *npolys = (int)np; return true;
}

bool BuildDetourTiles(const std::string& dir, int x0, int y0, int x1, int y1, dtNavMesh** out, PolyStats* stats, std::string& err) {
    struct TileData { unsigned char* data; int size; int x, y; };
    const float W = kZoneMetres;
    std::vector<TileData> tiles; int maxPolys = 1;
    for (int ty = y0; ty <= y1; ++ty) for (int tx = x0; tx <= x1; ++tx) {
        std::vector<Section> secs; if (!LoadTile(dir, tx, ty, secs, err)) return false;
        for (const Section& s : secs) {
            if (s.building) continue;
            unsigned char* data = nullptr; int dsz = 0, np = 0;
            if (!BuildTileData(s, tx, ty, &data, &dsz, &np, stats, err)) return false;
            if (!data) continue;
            tiles.push_back({data, dsz, tx, ty}); maxPolys = std::max(maxPolys, np);
        }
    }
    if (tiles.empty()) { err = "no ground sections in the region"; return false; }
    dtNavMesh* nav = dtAllocNavMesh(); dtNavMeshParams np2; std::memset(&np2, 0, sizeof np2);
    np2.tileWidth = W; np2.tileHeight = W; np2.maxTiles = (int)tiles.size(); np2.maxPolys = maxPolys;
    if (dtStatusFailed(nav->init(&np2))) { dtFreeNavMesh(nav); err = "navmesh init failed"; return false; }
    for (const TileData& t : tiles) if (dtStatusFailed(nav->addTile(t.data, t.size, DT_TILE_FREE_DATA, 0, nullptr))) { dtFreeNavMesh(nav); err = "addTile failed"; return false; }
    *out = nav; return true;
}

}  // namespace md::navtile

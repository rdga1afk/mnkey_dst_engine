#include <functional>
#include <monkey_dust/world/zone_buildings.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <unordered_map>

namespace md::world {

std::vector<std::string> ZoneFilesInCircle(const std::string& kenshi_dir, float cx_m, float cz_m, float radius_m) {
    std::vector<std::string> files;
    const float cx = cx_m * 10.0f, cz = cz_m * 10.0f, r = radius_m * 10.0f;
    const int x0 = (int)std::floor((cx - r) / ZONE_UNITS), x1 = (int)std::floor((cx + r) / ZONE_UNITS);
    const int z0 = (int)std::floor((cz - r) / ZONE_UNITS), z1 = (int)std::floor((cz + r) / ZONE_UNITS);
    const std::string root = kenshi_dir + "/data/newland/leveldata/";
    for (int zx = x0; zx <= x1; ++zx)
        for (int zz = z0; zz <= z1; ++zz)
            for (const char* folder : {"rebirth", "Newwworld"}) {
                std::string f = root + folder + "/zone." + std::to_string(zx) + "." + std::to_string(zz) + ".zone";
                if (std::filesystem::exists(f)) { files.push_back(std::move(f)); break; }
            }
    return files;
}

namespace {
struct BuildingDef { float scale = 1.0f; std::vector<std::string> meshes; std::vector<std::array<float, 3>> offsets; std::vector<std::string> tex, tex2; };   // offsets: the sum of the "offset X/Y/Z" of the part and of its parents (raw FCS values, not applied by the loader)
const float* FindFloat(const md::fcs::GameData& g, const char* key) { auto it = g.floats.find(key); return it == g.floats.end() ? nullptr : &it->second; }
}

// The part's offset chain moves its scene node: a child node at translation (offset X, Y, Z) of its parent, no own rotation or scale (exe 140848950, Ghidra 02.11: createChildSceneNode(offset, identity) when the
// part is not a position marker and the offset is non-zero; chains add). Under the instance node that is offset x instance scale, rotated by the instance rotation (w x y z); FCS units are 0.1 m.
static std::array<float, 3> OffsetWorld(const float* q_wxyz, float scale, const std::array<float, 3>& off) {
    const float n = std::sqrt(q_wxyz[0] * q_wxyz[0] + q_wxyz[1] * q_wxyz[1] + q_wxyz[2] * q_wxyz[2] + q_wxyz[3] * q_wxyz[3]);
    if (n < 1e-6f || (off[0] == 0.f && off[1] == 0.f && off[2] == 0.f)) return {off[0] * 0.1f * scale, off[1] * 0.1f * scale, off[2] * 0.1f * scale};
    const float w = q_wxyz[0] / n, x = q_wxyz[1] / n, y = q_wxyz[2] / n, z = q_wxyz[3] / n;
    const float vx = off[0] * 0.1f * scale, vy = off[1] * 0.1f * scale, vz = off[2] * 0.1f * scale;
    // v' = v + 2w (u x v) + 2 u x (u x v), u = (x, y, z)
    const float cx = y * vz - z * vy, cy = z * vx - x * vz, cz = x * vy - y * vx;
    const float dx = y * cz - z * cy, dy = z * cx - x * cz, dz = x * cy - y * cx;
    return {vx + 2.f * (w * cx + dx), vy + 2.f * (w * cy + dy), vz + 2.f * (w * cz + dz)};
}

namespace {
struct Circle { float cx, cz, r; };   // game units (0.1 m), origin-shifted

bool AppendZoneFile(const std::string& path, const md::fcs::GameDataContainer& data, const Circle* circle,
                    std::unordered_map<std::string, BuildingDef>& defs, std::vector<PlacedPart>& out, std::string& err) {
    auto def_of = [&](const std::string& id) -> const BuildingDef& {
        auto it = defs.find(id);
        if (it != defs.end()) return it->second;
        BuildingDef d;
        if (const md::fcs::GameData* b = data.Get(id); b && b->kind == 0) {
            if (const float* s = FindFloat(*b, "scale")) d.scale = *s;
            // a part may carry parts of its own (Storm_Shack_Exterior -> Storm_Shack_ExteriorROOF): walked to depth 8 like Okran render/zone.rs part_meshes_of
            std::function<void(const md::fcs::GameData&, int, std::array<float, 3>, const md::fcs::GameData*)> walk = [&](const md::fcs::GameData& node, int depth, std::array<float, 3> acc, const md::fcs::GameData* inherited) {
                const auto* parts = node.Refs("parts");
                if (!parts || depth > 8) return;
                for (const auto& p : *parts) {
                    const md::fcs::GameData* part = data.Get(p.first);
                    if (!part || part->kind != 29) continue;
                    std::array<float, 3> here = acc;
                    // the part's own MATERIAL_SPEC, else the building's default: the BUILDING's first `material`, else the record 742-gamedata.base (exe 140848950, read in Ghidra 02.11)
                    const md::fcs::GameData* mat = inherited;
                    if (const auto* mats = part->Refs("material")) if (!mats->empty()) if (const md::fcs::GameData* ms = data.Get(mats->front().first)) mat = ms;
                    if (const float* o = FindFloat(*part, "offset X")) here[0] += *o;
                    if (const float* o = FindFloat(*part, "offset Y")) here[1] += *o;
                    if (const float* o = FindFloat(*part, "offset Z")) here[2] += *o;
                    auto f = part->files.find("phs or mesh");
                    if (f != part->files.end() && !f->second.empty()) {
                        std::string m = f->second;
                        std::replace(m.begin(), m.end(), '\\', '/');
                        std::string low = m; std::transform(low.begin(), low.end(), low.begin(), [](unsigned char c) { return (char)std::tolower(c); });
                        if (low.find("black") == std::string::npos) {
                            std::string t1, t2;   // the part's first MATERIAL_SPEC: `texture map` (+ `texture map 2`)
                            if (const md::fcs::GameData* ms = mat) {
                                auto a = ms->files.find("texture map"); if (a != ms->files.end()) t1 = a->second;
                                auto b = ms->files.find("texture map 2"); if (b != ms->files.end()) t2 = b->second;
                                std::replace(t1.begin(), t1.end(), '\\', '/'); std::replace(t2.begin(), t2.end(), '\\', '/');
                            }
                            d.meshes.push_back(std::move(m)); d.offsets.push_back(here); d.tex.push_back(std::move(t1)); d.tex2.push_back(std::move(t2));
                        }
                    }
                    walk(*part, depth + 1, here, inherited);
                }
            };
            {
                const md::fcs::GameData* bmat = nullptr;
                if (const auto* mats = b->Refs("material")) if (!mats->empty()) bmat = data.Get(mats->front().first);
                if (!bmat) bmat = data.Get("742-gamedata.base");   // the exe's default MATERIAL_SPEC for a building without one
                walk(*b, 0, {0.f, 0.f, 0.f}, bmat);
            }
        }
        return defs.emplace(id, std::move(d)).first->second;
    };
    md::fcs::File file; md::fcs::Error e;
    if (!md::fcs::ReadFile(path, file, e)) { err = e.Message(); return false; }
    std::unordered_map<std::string, const md::fcs::Record*> by_id;
    const md::fcs::Record* list = nullptr;
    for (const auto& rec : file.records) {
        by_id[rec.string_id] = &rec;
        if (rec.string_id == "0-buildinglist") list = &rec;
    }
    if (!list) return true;
    for (const auto& in : list->instances) {
        if (std::fabs(in.position[0]) + std::fabs(in.position[2]) < 1.0f || in.target.empty()) continue;
        if (circle) {
            const float dx = in.position[0] - circle->cx, dz = in.position[2] - circle->cz;
            if (dx * dx + dz * dz > circle->r * circle->r) continue;
        }
        if (in.states.empty()) continue;
        auto zr = by_id.find(in.states[0]);
        if (zr == by_id.end()) continue;
        float wy = 0; bool have = false;
        for (const auto& f : zr->second->floats) if (f.first == "world Y pos") { wy = f.second; have = true; }
        if (!have) continue;
        const BuildingDef& d = def_of(in.target);
        for (size_t mi = 0; mi < d.meshes.size(); ++mi) {
            const std::array<float, 3> ow = OffsetWorld(in.rotation, d.scale, d.offsets[mi]);
            out.push_back({d.meshes[mi], (float)(((double)in.position[0] + WORLD_ORIGIN_UNITS) / 10.0) + ow[0], (float)((double)wy / 10.0) + ow[1], (float)(((double)in.position[2] + WORLD_ORIGIN_UNITS) / 10.0) + ow[2],
                           in.position[1] / 10.0f, d.scale, {in.rotation[0], in.rotation[1], in.rotation[2], in.rotation[3]}, {d.offsets[mi][0], d.offsets[mi][1], d.offsets[mi][2]}, d.tex[mi], d.tex2[mi]});
        }
    }
    return true;
}
} // namespace

bool LoadBuildingsInCircle(const std::string& kenshi_dir, const md::fcs::GameDataContainer& data,
                           float cx_m, float cz_m, float radius_m, std::vector<PlacedPart>& out, std::string& err) {
    std::unordered_map<std::string, BuildingDef> defs;
    const Circle c = { cx_m * 10.0f - WORLD_ORIGIN_UNITS, cz_m * 10.0f - WORLD_ORIGIN_UNITS, radius_m * 10.0f };
    for (const std::string& path : ZoneFilesInCircle(kenshi_dir, cx_m, cz_m, radius_m))
        if (!AppendZoneFile(path, data, &c, defs, out, err)) return false;
    return true;
}

bool LoadZoneBuildings(const std::string& kenshi_dir, const md::fcs::GameDataContainer& data, int zx, int zz,
                       std::vector<PlacedPart>& out, std::string& err) {
    std::unordered_map<std::string, BuildingDef> defs;
    const std::string root = kenshi_dir + "/data/newland/leveldata/";
    for (const char* folder : {"rebirth", "Newwworld"}) {
        const std::string f = root + folder + "/zone." + std::to_string(zx) + "." + std::to_string(zz) + ".zone";
        if (std::filesystem::exists(f)) return AppendZoneFile(f, data, nullptr, defs, out, err);
    }
    return true;
}

bool LoadBuildingsAt(const std::string& kenshi_dir, float cx_m, float cz_m, float radius_m, std::vector<PlacedPart>& out, std::string& err) {
    md::fcs::Install inst(kenshi_dir);
    md::fcs::LoadOrder order; md::fcs::Error e;
    if (!inst.LoadOrderFor(order, e)) { err = e.Message(); return false; }
    md::fcs::GameDataContainer data;
    data.replace_reference_lists = true;
    if (!data.Load(order, e)) { err = e.Message(); return false; }
    return LoadBuildingsInCircle(kenshi_dir, data, cx_m, cz_m, radius_m, out, err);
}

} // namespace md::world

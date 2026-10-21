#include <monkey_dust/world/zone_buildings.h>
#include <algorithm>
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
struct BuildingDef { float scale = 1.0f; std::vector<std::string> meshes; };
const float* FindFloat(const md::fcs::GameData& g, const char* key) { auto it = g.floats.find(key); return it == g.floats.end() ? nullptr : &it->second; }
}

bool LoadBuildingsInCircle(const std::string& kenshi_dir, const md::fcs::GameDataContainer& data,
                           float cx_m, float cz_m, float radius_m, std::vector<PlacedPart>& out, std::string& err) {
    std::unordered_map<std::string, BuildingDef> defs;
    auto def_of = [&](const std::string& id) -> const BuildingDef& {
        auto it = defs.find(id);
        if (it != defs.end()) return it->second;
        BuildingDef d;
        if (const md::fcs::GameData* b = data.Get(id); b && b->kind == 0) {
            if (const float* s = FindFloat(*b, "scale")) d.scale = *s;
            if (const auto* parts = b->Refs("parts"))
                for (const auto& p : *parts) {
                    const md::fcs::GameData* part = data.Get(p.first);
                    if (!part || part->kind != 29) continue;
                    auto f = part->files.find("phs or mesh");
                    if (f == part->files.end() || f->second.empty()) continue;
                    std::string m = f->second;
                    std::replace(m.begin(), m.end(), '\\', '/');
                    std::string low = m; std::transform(low.begin(), low.end(), low.begin(), [](unsigned char c) { return (char)std::tolower(c); });
                    if (low.find("black") != std::string::npos) continue;
                    d.meshes.push_back(std::move(m));
                }
        }
        return defs.emplace(id, std::move(d)).first->second;
    };
    const float cx = cx_m * 10.0f - WORLD_ORIGIN_UNITS, cz = cz_m * 10.0f - WORLD_ORIGIN_UNITS, r = radius_m * 10.0f;
    for (const std::string& path : ZoneFilesInCircle(kenshi_dir, cx_m, cz_m, radius_m)) {
        md::fcs::File file; md::fcs::Error e;
        if (!md::fcs::ReadFile(path, file, e)) { err = e.Message(); return false; }
        std::unordered_map<std::string, const md::fcs::Record*> by_id;
        const md::fcs::Record* list = nullptr;
        for (const auto& rec : file.records) {
            by_id[rec.string_id] = &rec;
            if (rec.string_id == "0-buildinglist") list = &rec;
        }
        if (!list) continue;
        for (const auto& in : list->instances) {
            if (std::fabs(in.position[0]) + std::fabs(in.position[2]) < 1.0f || in.target.empty()) continue;
            const float dx = in.position[0] - cx, dz = in.position[2] - cz;
            if (dx * dx + dz * dz > r * r) continue;
            if (in.states.empty()) continue;
            auto zr = by_id.find(in.states[0]);
            if (zr == by_id.end()) continue;
            float wy = 0; bool have = false;
            for (const auto& f : zr->second->floats) if (f.first == "world Y pos") { wy = f.second; have = true; }
            if (!have) continue;
            const BuildingDef& d = def_of(in.target);
            for (const std::string& m : d.meshes)
                out.push_back({m, (in.position[0] + WORLD_ORIGIN_UNITS) / 10.0f, wy / 10.0f, (in.position[2] + WORLD_ORIGIN_UNITS) / 10.0f,
                               in.position[1] / 10.0f, d.scale, {in.rotation[0], in.rotation[1], in.rotation[2], in.rotation[3]}});
        }
    }
    return true;
}

bool LoadBuildingsAt(const std::string& kenshi_dir, float cx_m, float cz_m, float radius_m, std::vector<PlacedPart>& out, std::string& err) {
    md::fcs::Install inst(kenshi_dir);
    md::fcs::LoadOrder order; md::fcs::Error e;
    if (!inst.LoadOrderFor(order, e)) { err = e.Message(); return false; }
    md::fcs::GameDataContainer data;
    if (!data.Load(order, e)) { err = e.Message(); return false; }
    return LoadBuildingsInCircle(kenshi_dir, data, cx_m, cz_m, radius_m, out, err);
}

} // namespace md::world

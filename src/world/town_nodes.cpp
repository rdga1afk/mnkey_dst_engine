#include <monkey_dust/world/town_nodes.h>
#include <monkey_dust/world/zone_buildings.h>
#include <cmath>
#include <unordered_map>

namespace md::world {
namespace {
struct LocalNode { int type; float p[3]; };

void Rotate(const float* q_wxyz, const float* v, float* o) {   // v' = v + 2w (u x v) + 2 u x (u x v), u = (x, y, z) of the normalised w x y z quaternion
    const float n = std::sqrt(q_wxyz[0] * q_wxyz[0] + q_wxyz[1] * q_wxyz[1] + q_wxyz[2] * q_wxyz[2] + q_wxyz[3] * q_wxyz[3]);
    if (n < 1e-6f) { o[0] = v[0]; o[1] = v[1]; o[2] = v[2]; return; }
    const float w = q_wxyz[0] / n, x = q_wxyz[1] / n, y = q_wxyz[2] / n, z = q_wxyz[3] / n;
    const float cx = y * v[2] - z * v[1], cy = z * v[0] - x * v[2], cz = x * v[1] - y * v[0];
    const float dx = y * cz - z * cy, dy = z * cx - x * cz, dz = x * cy - y * cx;
    o[0] = v[0] + 2.f * (w * cx + dx); o[1] = v[1] + 2.f * (w * cy + dy); o[2] = v[2] + 2.f * (w * cz + dz);
}
}  // namespace

bool LoadTownSites(const std::string& kenshi_dir, const md::fcs::GameDataContainer& data, std::vector<TownSite>& out, std::string& err) {
    std::unordered_map<std::string, size_t> at;
    for (const char* folder : { "Newwworld", "rebirth" }) {
        md::fcs::File file; md::fcs::Error e;
        if (!md::fcs::ReadFile(kenshi_dir + "/data/newland/leveldata/" + folder + "/leveldata.level", file, e)) { err = e.Message(); return false; }
        for (const auto& rec : file.records) {
            if (rec.kind != 38) continue;
            for (const auto& in : rec.instances) {
                const md::fcs::GameData* t = data.Get(in.target); if (!t || t->kind != 13) continue;
                TownSite s; s.id = in.target; s.x = (float)(((double)in.position[0] + WORLD_ORIGIN_UNITS) / 10.0); s.z = (float)(((double)in.position[2] + WORLD_ORIGIN_UNITS) / 10.0);
                auto ty = t->ints.find("type"); s.type = ty == t->ints.end() ? 0 : ty->second;
                auto mu = t->floats.find("town radius mult"); s.mult = mu == t->floats.end() ? 1.f : mu->second;
                auto it = at.find(in.target);
                if (it == at.end()) { at[in.target] = out.size(); out.push_back(s); } else out[it->second] = s;
            }
        }
    }
    return true;
}

bool LoadTownNodes(const std::string& kenshi_dir, const md::fcs::GameDataContainer& data, const std::vector<TownSite>& towns, int town, std::vector<TownNode>& out, std::string& err) {
    if (town < 0 || (size_t)town >= towns.size()) return true;
    const float cx_m = towns[town].x, cz_m = towns[town].z, radius_m = TownRadiusM(towns[town].type, towns[town].mult);
    md::fcs::File interiors; md::fcs::Error e;
    if (!md::fcs::ReadFile(kenshi_dir + "/data/newland/leveldata/interiors.level", interiors, e)) { err = e.Message(); return false; }
    std::unordered_map<std::string, std::vector<LocalNode>> by_building;   // BUILDING id -> its type 0 / 1 nodes (local, FCS units)
    for (const auto& rec : interiors.records) {
        std::string building;
        for (const auto& s : rec.strings) if (s.first == "associated building") building = s.second;
        if (building.empty() || rec.instances.empty()) continue;
        for (const auto& in : rec.instances) {
            const md::fcs::GameData* g = data.Get(in.target); if (!g) continue;
            auto isn = g->bools.find("is node"); if (isn == g->bools.end() || !isn->second) continue;
            auto nt = g->ints.find("node type"); if (nt == g->ints.end() || (nt->second < 0 || nt->second > 2)) continue;
            by_building[building].push_back({ nt->second, { in.position[0], in.position[1], in.position[2] } });
        }
    }
    int placed_n = 0;
    for (const std::string& path : ZoneFilesInCircle(kenshi_dir, cx_m, cz_m, radius_m)) {
        md::fcs::File zone;
        if (!md::fcs::ReadFile(path, zone, e)) { err = e.Message(); return false; }
        std::unordered_map<std::string, const md::fcs::Record*> by_id; const md::fcs::Record* list = nullptr;
        for (const auto& rec : zone.records) { by_id[rec.string_id] = &rec; if (rec.string_id == "0-buildinglist") list = &rec; }
        if (!list) continue;
        for (const auto& in : list->instances) {
            if (std::fabs(in.position[0]) + std::fabs(in.position[2]) < 1.0f || in.target.empty() || in.states.empty()) continue;
            const float bx = (float)(((double)in.position[0] + WORLD_ORIGIN_UNITS) / 10.0), bz = (float)(((double)in.position[2] + WORLD_ORIGIN_UNITS) / 10.0);
            if (TownOfPoint(towns, bx, bz) != town) continue;
            auto nodes = by_building.find(in.target); if (nodes == by_building.end()) continue;
            auto zr = by_id.find(in.states[0]); if (zr == by_id.end()) continue;
            float wy = 0; bool have = false;
            for (const auto& f : zr->second->floats) if (f.first == "world Y pos") { wy = f.second; have = true; }
            if (!have) continue;
            float scale = 1.f; if (const md::fcs::GameData* b = data.Get(in.target)) { auto s = b->floats.find("scale"); if (s != b->floats.end() && s->second > 0.f) scale = s->second; }
            const int placed = placed_n++;
            for (const LocalNode& n : nodes->second) {
                const float v[3] = { n.p[0] * 0.1f * scale, n.p[1] * 0.1f * scale, n.p[2] * 0.1f * scale }; float r[3];
                Rotate(in.rotation, v, r);
                TownNode t; t.building = in.target; t.node_type = n.type; t.placed = placed;
                t.x = (float)(((double)in.position[0] + WORLD_ORIGIN_UNITS) / 10.0) + r[0];
                t.y = (float)((double)wy / 10.0) + in.position[1] / 10.0f + r[1];
                t.z = (float)(((double)in.position[2] + WORLD_ORIGIN_UNITS) / 10.0) + r[2];
                out.push_back(std::move(t));
            }
        }
    }
    return true;
}

}  // namespace md::world

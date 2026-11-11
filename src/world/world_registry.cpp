#include <monkey_dust/world/world_registry.h>
#include <monkey_dust/world/biome_system.h>
#include <monkey_dust/io/kenshi_dir.h>
#include <monkey_dust/io/data_files.h>
#include <stb_image.h>
#include <string>
#include <vector>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <dirent.h>
#include <monkey_dust/io/fcs_gamedata.h>

int WorldRegistry::BuildZonesFromInstall() {
    const std::string dir = md::KenshiDir();
    if (dir.empty()) return 0;
    md::fcs::Install inst(dir); md::fcs::LoadOrder order; md::fcs::Error e; md::fcs::GameDataContainer gd;
    if (!inst.LoadOrderFor(order, e) || !gd.Load(order, e)) return 0;
    int w = 0, h = 0, ch = 0;
    unsigned char* areas = stbi_load((dir + "/data/newland/land/areasmap.tga").c_str(), &w, &h, &ch, 3);
    if (!areas) return 0;
    zone_count_ = 0;
    for (const md::fcs::GameData* gr : gd.OfType(95)) {
        const auto it = gr->ints.find("index");
        if (it == gr->ints.end() || gr->name == "NONE") continue;
        const uint32_t key = (uint32_t)it->second & 0xFFFFFFu;
        double sx = 0, sz = 0; long n = 0;
        for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x) {
            const unsigned char* p = areas + ((size_t)y * w + x) * 3;
            if ((((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2]) == key) { sx += x; sz += y; ++n; }
        }
        if (n == 0 || zone_count_ >= WR_MAX_ZONES) continue;
        const double cx = sx / (double)n, cz = sz / (double)n;
        long best_d = -1; int bx = 0, bz = 0;
        for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x) {
            const unsigned char* p = areas + ((size_t)y * w + x) * 3;
            if ((((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2]) != key) continue;
            const long d = (long)(((double)x - cx) * ((double)x - cx) + ((double)y - cz) * ((double)y - cz));
            if (best_d < 0 || d < best_d) { best_d = d; bx = x; bz = y; }
        }
        ZoneRecord& z = zones_[zone_count_++];
        z = ZoneRecord{};
        strncpy(z.display_name, gr->name.c_str(), WR_NAME_LEN - 1);
        std::string slug; bool gap = false;
        for (char c : gr->name) {
            if (isalnum((unsigned char)c)) { if (gap && !slug.empty()) slug += '_'; slug += (char)tolower((unsigned char)c); gap = false; } else gap = true;
        }
        strncpy(z.name, slug.c_str(), WR_NAME_LEN - 1);
        z.grid_x = std::min(bx * 64 / w, 63); z.grid_z = std::min(bz * 64 / h, 63);
    }
    stbi_image_free(areas);
    fprintf(stdout, "[WorldRegistry] %d zones from the install (kind-95 groups + areasmap regions)\n", zone_count_);
    return zone_count_;
}

// The biome of every zone cell from the install (owner 11.11: the record is the truth, no hand classification): the BIOMES record whose colour field equals the colour of the cell centre
// in biomemap.png. Every distinct colour of the map has exactly one record (tests/test_biome_map_install.cpp); a colour without one falls back to record 0 and is logged.
bool WorldRegistry::BuildBiomeMapFromInstall() {
    const std::string dir = md::KenshiDir();
    if (dir.empty()) return false;
    int w = 0, h = 0, ch = 0;
    unsigned char* px = stbi_load((dir + "/data/newland/land/biomemap.png").c_str(), &w, &h, &ch, 4);
    if (!px) return false;
    BiomeRegistry& R = BiomeRegistry::Get();
    if (!R.Loaded()) R.LoadFromKenshi(dir.c_str(), (dir + "/data/newland/land/textures").c_str());
    if (!R.Loaded()) { stbi_image_free(px); return false; }
    int missing = 0;
    for (int zz = 0; zz < 64; ++zz) for (int zx = 0; zx < 64; ++zx) {
        const unsigned char* q = px + 4 * ((size_t)std::min(zz * 16 + 8, h - 1) * w + std::min(zx * 16 + 8, w - 1));
        const int id = R.FindByLegend((uint32_t)q[0] << 16 | (uint32_t)q[1] << 8 | q[2]);
        if (id < 0) ++missing;
        biome_grid_[zz][zx] = (uint8_t)(id < 0 ? 0 : id);
    }
    stbi_image_free(px);
    if (missing) fprintf(stderr, "[WorldRegistry] %d zone cells have a biomemap colour without a BIOMES record (record 0)\n", missing);
    fprintf(stdout, "[WorldRegistry] Biome map built from the BIOMES records of the install\n");
    return true;
}

// MD_WORLD_REGISTRY=<file> replaces the default registry path: gates and scenarios run on a fixture and never touch the player's game/data file
static const char* RegistryPath(const char* path) {
    const char* e = getenv("MD_WORLD_REGISTRY");
    return (e && *e) ? e : path;
}

void WorldRegistry::Load(const char* path) {
    FILE* fp = fopen(RegistryPath(path), "r");
    if (!fp) return;
    char line[128];
    while (fgets(line, sizeof(line), fp)) {
        if (line[0] == '#' || line[0] == '\n') continue;
        char key[80], val[64];
        if (sscanf(line, " %79[^=]=%63s", key, val) != 2) continue;

        if      (!strcmp(key, "current_zone")) strncpy(current_zone_, val, WR_NAME_LEN - 1);
        else if (!strcmp(key, "player_x"))     player_x_ = (float)atof(val);
        else if (!strcmp(key, "player_z"))     player_z_ = (float)atof(val);
        else if (strncmp(key, "zone_", 5) == 0) {
            // Split "zone_<name>_<field>" on the last known field suffix.
            int klen = (int)strlen(key);
            static const struct { const char* sfx; int slen; } kSuf[] = {
                {"_visited", 8}, {"_exit_x", 7}, {"_exit_z", 7}, {"_amp", 4}
            };
            for (int s = 0; s < 4; ++s) {
                int slen = kSuf[s].slen;
                if (klen <= 5 + slen) continue;
                if (strcmp(key + klen - slen, kSuf[s].sfx) != 0) continue;
                int znlen = klen - 5 - slen;
                if (znlen <= 0 || znlen >= WR_NAME_LEN) break;
                char zname[WR_NAME_LEN];
                memcpy(zname, key + 5, (size_t)znlen);
                zname[znlen] = '\0';
                ZoneRecord* r = GetOrCreate(zname);
                if (!r) break;
                const char* field = kSuf[s].sfx + 1;
                if      (!strcmp(field, "visited")) r->visited   = atoi(val) != 0;
                else if (!strcmp(field, "exit_x"))  r->exit_x    = (float)atof(val);
                else if (!strcmp(field, "exit_z"))  r->exit_z    = (float)atof(val);
                else if (!strcmp(field, "amp"))     r->amplitude = (float)atof(val);
                break;
            }
        }
    }
    fclose(fp);
}


void WorldRegistry::Save(const char* path) const {
    FILE* fp = fopen(RegistryPath(path), "w");
    if (!fp) return;
    fprintf(fp, "# monkey_dust world registry — auto-generated\n");
    fprintf(fp, "current_zone=%s\n", current_zone_);
    fprintf(fp, "player_x=%.3f\n",  player_x_);
    fprintf(fp, "player_z=%.3f\n",  player_z_);
    for (int i = 0; i < zone_count_; ++i) {
        const ZoneRecord& r = zones_[i];
        if (r.name[0] == '\0') continue;
        fprintf(fp, "zone_%s_visited=%d\n",  r.name, r.visited ? 1 : 0);
        fprintf(fp, "zone_%s_exit_x=%.3f\n", r.name, r.exit_x);
        fprintf(fp, "zone_%s_exit_z=%.3f\n", r.name, r.exit_z);
        fprintf(fp, "zone_%s_amp=%.1f\n",    r.name, r.amplitude);
    }
    fclose(fp);
}

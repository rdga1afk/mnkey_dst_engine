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

void WorldRegistry::LoadFromTerrainConfig(const char* path) {
    FILE* fp = fopen(md::DataPath(path).c_str(), "r");
    if (!fp) {
        fprintf(stderr, "[WorldRegistry] terrain_config not found: %s\n", path);
        return;
    }
    char line[256];
    ZoneRecord* cur = nullptr;
    while (fgets(line, sizeof(line), fp)) {
        const char* p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '#' || *p == '\n' || *p == '\r' || *p == '\0') continue;
        if (*p == '[') continue;

        if (strncmp(p, "zone=", 5) == 0) {
            if (zone_count_ >= WR_MAX_ZONES) break;
            cur = &zones_[zone_count_++];
            *cur = ZoneRecord{};
            cur->amplitude   = 30.f;
            cur->fog_density = 0.3f;
            cur->fog_dist_m  = 800.f;
            cur->danger      = 1;
            const char* id = p + 5;
            int len = 0;
            while (id[len] && id[len] != '\n' && id[len] != '\r' && id[len] != ' ') len++;
            if (len >= WR_NAME_LEN) len = WR_NAME_LEN - 1;
            memcpy(cur->name, id, (size_t)len);
            cur->name[len] = '\0';
            continue;
        }

        if (!cur) continue;
        const char* eq = strchr(p, '=');
        if (!eq) continue;

        char key[64];
        int klen = (int)(eq - p);
        while (klen > 0 && (p[klen-1] == ' ' || p[klen-1] == '\t')) klen--;
        if (klen <= 0 || klen >= 64) continue;
        memcpy(key, p, (size_t)klen); key[klen] = '\0';

        const char* vs = eq + 1;
        while (*vs == ' ' || *vs == '\t') vs++;
        int vlen = 0;
        while (vs[vlen] && vs[vlen] != '\n' && vs[vlen] != '\r') vlen++;
        while (vlen > 0 && (vs[vlen-1] == ' ' || vs[vlen-1] == '\t')) vlen--;
        char val[128];
        if (vlen >= 128) vlen = 127;
        memcpy(val, vs, (size_t)vlen); val[vlen] = '\0';

        if      (!strcmp(key, "name"))        strncpy(cur->display_name, val, WR_NAME_LEN - 1);
        else if (!strcmp(key, "biome"))       strncpy(cur->biome,        val, sizeof(cur->biome) - 1);
        else if (!strcmp(key, "amplitude"))   cur->amplitude   = (float)atof(val);
        else if (!strcmp(key, "danger"))      cur->danger      = atoi(val);
        else if (!strcmp(key, "grid_x"))      cur->grid_x      = atoi(val);
        else if (!strcmp(key, "grid_z"))      cur->grid_z      = atoi(val);
        else if (!strcmp(key, "fog_density")) cur->fog_density = (float)atof(val);
        else if (!strcmp(key, "fog_dist"))    cur->fog_dist_m  = (float)atof(val);
        else if (!strcmp(key, "factions"))    strncpy(cur->factions_str, val, sizeof(cur->factions_str) - 1);
    }
    fclose(fp);

    ZoneRecord* active = Find(current_zone_);
    if (active) active->visited = true;
    fprintf(stdout, "[WorldRegistry] Loaded %d zones from terrain_config\n", zone_count_);
}

constexpr const char* WorldRegistry::kBiomeNames[];

// The biome of every zone cell from the install's biomemap.png (tools/md_biome_import.py, now at run time): the zones of terrain_config.txt are the colour anchors, every other cell takes the nearest anchor colour
bool WorldRegistry::BuildBiomeMapFromInstall() {
    const std::string dir = md::KenshiDir();
    if (dir.empty()) return false;
    int w = 0, h = 0, ch = 0;
    unsigned char* px = stbi_load((dir + "/data/newland/land/biomemap.png").c_str(), &w, &h, &ch, 4);
    if (!px) return false;
    auto sample = [&](int zx, int zz) {
        const int x = std::min(zx * 16 + 8, w - 1), y = std::min(zz * 16 + 8, h - 1);
        const unsigned char* q = px + 4 * ((size_t)y * w + x);
        return (uint32_t)q[0] << 16 | (uint32_t)q[1] << 8 | q[2];
    };
    struct Anchor { uint32_t rgb; uint8_t biome; };
    std::vector<Anchor> anchors;   // insertion order of the first use of a colour; a later zone with the same colour replaces its biome
    bool known[64][64] = {};
    auto biome_index = [&](const char* name) { for (uint8_t i = 0; i < 11; ++i) if (!strcmp(name, kBiomeNames[i])) return i; return (uint8_t)0; };
    for (int i = 0; i < zone_count_; ++i) {
        const ZoneRecord& z = zones_[i];
        if (!z.biome[0] || z.grid_x < 0 || z.grid_x > 63 || z.grid_z < 0 || z.grid_z > 63) continue;
        const uint32_t c = sample(z.grid_x, z.grid_z); const uint8_t b = biome_index(z.biome);
        bool found = false;
        for (Anchor& a : anchors) if (a.rgb == c) { a.biome = b; found = true; break; }
        if (!found) anchors.push_back({ c, b });
        known[z.grid_z][z.grid_x] = true; biome_grid_[z.grid_z][z.grid_x] = b;
    }
    for (int zz = 0; zz < 64; ++zz) for (int zx = 0; zx < 64; ++zx) {
        if (known[zz][zx]) continue;
        const uint32_t c = sample(zx, zz); uint8_t best = biome_index("desert"); long best_d = -1;
        for (const Anchor& a : anchors) {
            if (a.rgb == c) { best = a.biome; break; }
            const long dr = (long)(c >> 16 & 255) - (long)(a.rgb >> 16 & 255), dg = (long)(c >> 8 & 255) - (long)(a.rgb >> 8 & 255), db = (long)(c & 255) - (long)(a.rgb & 255);
            const long d = dr * dr + dg * dg + db * db;
            if (best_d < 0 || d < best_d) { best_d = d; best = a.biome; }
        }
        biome_grid_[zz][zx] = best;
    }
    stbi_image_free(px);
    return true;
}

void WorldRegistry::LoadBiomeMap(const char* path) {
    // the install's biomemap.png first (fresh, tools/md_biome_import.py at run time); the committed table is only the fallback without an install
    if (BuildBiomeMapFromInstall()) { fprintf(stdout, "[WorldRegistry] Biome map built from the Kenshi install (biomemap.png)\n"); return; }
    FILE* fp = fopen(path, "r");
    if (!fp) return;
    char line[64];
    while (fgets(line, sizeof(line), fp)) {
        if (line[0] == '#' || line[0] == '\n') continue;
        int zx, zz;
        char biome[24] = {};
        if (sscanf(line, "%d,%d=%23s", &zx, &zz, biome) != 3) continue;
        if (zx < 0 || zx > 63 || zz < 0 || zz > 63) continue;
        uint8_t idx = 0;
        for (uint8_t i = 0; i < 11; ++i)
            if (!strcmp(biome, kBiomeNames[i])) { idx = i; break; }
        biome_grid_[zz][zx] = idx;
    }
    fclose(fp);
    fprintf(stdout, "[WorldRegistry] Biome map loaded from %s\n", path);
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

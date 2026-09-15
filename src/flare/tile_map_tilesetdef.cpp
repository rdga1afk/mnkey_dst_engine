// Tilesetdef parsing + atlas-path resolution (Phase 5 split, 2026-09-15,
// extracted from tile_map.cpp). Own format, used by both the .txt and .tmx
// map loaders and by LoadFlareMap/InitEmptyFlareMap directly.
#include "tile_map_internal.h"
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <monkey_dust/compat/md_dirent.h>
#include <sys/stat.h>

namespace md::flare {

// ── Tilesetdef parser ─────────────────────────────────────────────────────────

static bool ParseTilesetDefFile(const char* path, TileMetaRegistry& meta) {
    FILE* f = fopen(path, "r");
    if (!f) return false;
    char line[256];
    // Tracks which [tileset] section we are currently in.
    // Each img= line opens a new section; subsequent tile= entries belong to it.
    int cur_atlas = 0;
    while (fgets(line, sizeof(line), f)) {
        int len = (int)strlen(line);
        while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r')) line[--len] = '\0';

        if (strncmp(line, "img=", 4) == 0) {
            // Each img= opens a new atlas section (M7.23 multi-atlas).
            // Tile IDs in this section sample from this specific image.
            // We store all paths and tag each TileMeta with its section index.
            if (meta.atlas_count < MAX_ATLAS_COUNT) {
                strncpy(meta.atlas_paths[meta.atlas_count], line + 4, 255);
                meta.atlas_paths[meta.atlas_count][255] = '\0';
                cur_atlas = meta.atlas_count;
                ++meta.atlas_count;
                // atlas_rel_path mirrors atlas_paths[0] for backward compat.
                if (meta.atlas_count == 1)
                    strncpy(meta.atlas_rel_path, meta.atlas_paths[0],
                            sizeof(meta.atlas_rel_path) - 1);
            }
            continue;
        }
        // animation=id;sx,sy,Nms;sx,sy,Nms;…
        // Frames override src_x/src_y of the base tile= for each time slice.
        // w/h/atlas_idx stay the same as the corresponding TileMeta entry.
        if (strncmp(line, "animation=", 10) == 0) {
            const char* p = line + 10;
            int tid = atoi(p);
            while (*p && *p != ';') ++p;
            TileAnim anim;
            anim.tile_id    = (uint16_t)tid;
            anim.frame_count = 0;
            anim.total_ms   = 0;
            anim.atlas_idx  = (uint8_t)cur_atlas;
            // atlas_idx may be more precisely known from the base TileMeta.
            const TileMeta* base = meta.Find((uint16_t)tid);
            if (base) anim.atlas_idx = base->atlas_idx;

            while (*p == ';' && anim.frame_count < MAX_ANIM_FRAMES) {
                ++p;  // skip ';'
                if (!*p) break;
                int sx = atoi(p);
                while (*p && *p != ',') ++p;
                if (*p == ',') ++p;
                int sy = atoi(p);
                while (*p && *p != ',') ++p;
                if (*p == ',') ++p;
                int dur = atoi(p);  // digits before 'ms'
                while (*p && *p != ';') ++p;
                if (dur <= 0) continue;
                TileAnimFrame& f       = anim.frames[anim.frame_count++];
                f.src_x                = (int16_t)sx;
                f.src_y                = (int16_t)sy;
                f.duration_ms          = (uint32_t)dur;
                anim.total_ms         += (uint32_t)dur;
            }
            if (anim.frame_count > 1 && anim.total_ms > 0)
                meta.AddAnim(anim);
            continue;
        }

        if (strncmp(line, "tile=", 5) != 0) continue;
        const char* p = line + 5;
        int id, sx, sy, w, h, ox, oy;
        if (sscanf(p, "%d,%d,%d,%d,%d,%d,%d", &id, &sx, &sy, &w, &h, &ox, &oy) == 7) {
            TileMeta m;
            m.tile_id   = (uint16_t)id;
            m.src_x     = (int16_t)sx;
            m.src_y     = (int16_t)sy;
            m.w         = (int16_t)w;
            m.h         = (int16_t)h;
            m.offset_x  = (int16_t)ox;
            m.offset_y  = (int16_t)oy;
            m.atlas_idx = (uint8_t)cur_atlas;
            m._pad2     = 0;
            meta.Add(m);
        } else {
            fprintf(stderr, "[TileMeta] bad format: %s", line);
        }
    }
    fclose(f);
    return meta.count > 0;
}

// Resolve one atlas relative path by searching all sibling mod directories,
// then falling back to the same mod dir.  Returns true if found.
bool ResolveAtlasInMods(const char* mods_root, const char* mod_dir,
                         const char* rel_path, char* out, int out_n) {
    out[0] = '\0';
    if (!rel_path || !rel_path[0]) return false;
    DIR* d = opendir(mods_root);
    if (d) {
        struct dirent* ent;
        while ((ent = readdir(d)) != nullptr) {
            if (ent->d_name[0] == '.') continue;
            char candidate[512];
            snprintf(candidate, sizeof(candidate), "%s/%s/%s",
                     mods_root, ent->d_name, rel_path);
            struct stat st;
            if (stat(candidate, &st) == 0 && S_ISREG(st.st_mode)) {
                strncpy(out, candidate, (size_t)(out_n - 1));
                out[out_n - 1] = '\0';
                closedir(d);
                return true;
            }
        }
        closedir(d);
    }
    char candidate[512];
    snprintf(candidate, sizeof(candidate), "%s/%s", mod_dir, rel_path);
    struct stat st;
    if (stat(candidate, &st) == 0 && S_ISREG(st.st_mode)) {
        strncpy(out, candidate, (size_t)(out_n - 1));
        out[out_n - 1] = '\0';
        return true;
    }
    return false;
}

// Extract parent directory in-place (removes last path component).
void DirOf(const char* path, char* out, int n) {
    snprintf(out, (size_t)n, "%s", path);
    char* last = nullptr;
    for (char* c = out; *c; ++c) if (*c == '/') last = c;
    if (last) *last = '\0';
    else { out[0] = '.'; out[1] = '\0'; }
}

// Scan siblings of the map's mod directory for a tilesetdef file.
// Map is at: {mods_root}/{mod}/{maps}/{file}.txt
// tileset_def is relative to a mod root, e.g. "tilesetdefs/tileset_grassland.txt".
bool TryLoadTilesetDef(const char* map_path, const char* tileset_def,
                        TileMetaRegistry& meta) {
    if (!tileset_def || !tileset_def[0]) return false;

    char map_dir[512], mod_dir[512], mods_root[512];
    DirOf(map_path, map_dir, sizeof(map_dir));   // strip filename → .../maps
    DirOf(map_dir, mod_dir, sizeof(mod_dir));    // strip maps/  → .../mod
    DirOf(mod_dir, mods_root, sizeof(mods_root)); // strip mod/  → .../mods

    DIR* d = opendir(mods_root);
    if (!d) {
        // Fallback: try same mod dir
        char candidate[512];
        snprintf(candidate, sizeof(candidate), "%s/%s", mod_dir, tileset_def);
        return ParseTilesetDefFile(candidate, meta);
    }

    struct dirent* ent;
    bool found = false;
    while (!found && (ent = readdir(d)) != nullptr) {
        if (ent->d_name[0] == '.') continue;
        char candidate[512];
        snprintf(candidate, sizeof(candidate), "%s/%s/%s",
                 mods_root, ent->d_name, tileset_def);
        struct stat st;
        if (stat(candidate, &st) == 0 && S_ISREG(st.st_mode)) {
            fprintf(stdout, "[TileMeta] loading tilesetdef: %s\n", candidate);
            found = ParseTilesetDefFile(candidate, meta);
        }
    }
    closedir(d);
    return found;
}

} // namespace md::flare

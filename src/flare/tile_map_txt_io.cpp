// Flare-native .txt map format loader (Phase 5 split, 2026-09-15, extracted
// from tile_map.cpp).
#include "tile_map_internal.h"
#include <cstdio>
#include <cstring>
#include <cstdlib>

namespace md::flare {

// Parse one CSV line of tile IDs into layer.tiles[row * MAX_MAP_WIDTH + col].
static void ParseTileRow(const char* p, TileMapLayer& layer, int row, int map_w) {
    if (row < 0 || row >= MAX_MAP_HEIGHT) return;
    int col = 0;
    while (*p && col < map_w && col < MAX_MAP_WIDTH) {
        while (*p == ' ' || *p == '\t') ++p;
        if (!*p || *p == '\n' || *p == '\r') break;
        long v = strtol(p, (char**)&p, 10);
        layer.tiles[row * MAX_MAP_WIDTH + col] = (uint16_t)(v > 0 ? v : 0);
        ++col;
        if (*p == ',') ++p;
    }
}

bool LoadTxt(const char* path, FlareMap& m) {
    FILE* f = fopen(path, "r");
    if (!f) return false;

    memset(&m, 0, sizeof(m));
    m.tile_w = 192; m.tile_h = 96;

    enum { S_NONE, S_HEADER, S_TILESETS, S_LAYER, S_ENEMY, S_SKIP } state = S_NONE;

    TileMapLayer* cur_layer  = nullptr;
    bool          reading_data = false;
    int           tile_row     = 0;

    FlareSpawn cur_enemy;
    bool       enemy_open = false;
    memset(&cur_enemy, 0, sizeof(cur_enemy));

    char line[1024];
    while (fgets(line, sizeof(line), f)) {
        int len = (int)strlen(line);
        while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r')) line[--len] = '\0';

        if (len == 0) { reading_data = false; continue; }

        if (line[0] == '[') {
            char sec[32] = {};
            for (int i = 1; i < len && line[i] != ']' && i < 31; ++i) sec[i-1] = line[i];
            reading_data = false;

            // Commit pending enemy block before switching section.
            if (enemy_open && cur_enemy.category[0] && m.spawn_count < MAX_SPAWNS_PER_MAP) {
                m.spawns[m.spawn_count++] = cur_enemy;
            }
            enemy_open = false;

            if      (strcmp(sec, "header")   == 0) { state = S_HEADER; }
            else if (strcmp(sec, "tilesets") == 0) { state = S_TILESETS; }
            else if (strcmp(sec, "layer")    == 0) {
                state = S_LAYER;
                if (m.layer_count < MAX_MAP_LAYERS) {
                    cur_layer = &m.layers[m.layer_count++];
                    memset(cur_layer, 0, sizeof(*cur_layer));
                    cur_layer->type = LayerType::BACKGROUND;
                    tile_row = 0;
                } else {
                    cur_layer = nullptr;
                }
            }
            else if (strcmp(sec, "enemy") == 0) {
                state = S_ENEMY;
                memset(&cur_enemy, 0, sizeof(cur_enemy));
                cur_enemy.number_min = 1;
                enemy_open = true;
            }
            else { state = S_SKIP; cur_layer = nullptr; }
            continue;
        }

        if (state == S_SKIP) continue;

        if (reading_data && cur_layer) {
            ParseTileRow(line, *cur_layer, tile_row, m.width);
            ++tile_row;
            continue;
        }

        char* eq = strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';
        const char* key = line;
        const char* val = eq + 1;
        while (*val == ' ' || *val == '\t') ++val;

        if (state == S_HEADER) {
            if      (strcmp(key, "width")      == 0) m.width  = atoi(val);
            else if (strcmp(key, "height")     == 0) m.height = atoi(val);
            else if (strcmp(key, "tilewidth")  == 0) m.tile_w = atoi(val);
            else if (strcmp(key, "tileheight") == 0) m.tile_h = atoi(val);
            else if (strcmp(key, "music")      == 0) CpStr(m.music_path,  sizeof(m.music_path),  val);
            else if (strcmp(key, "tileset")    == 0) CpStr(m.tileset_def, sizeof(m.tileset_def), val);
            else if (strcmp(key, "title")      == 0) CpStr(m.title,       sizeof(m.title),       val);
            else if (strcmp(key, "hero_pos")   == 0) sscanf(val, "%f,%f", &m.hero_x, &m.hero_y);
        }
        else if (state == S_TILESETS && strcmp(key, "tileset") == 0) {
            if (m.tileset_count < MAX_TILESETS) {
                TileSet& ts = m.tilesets[m.tileset_count++];
                memset(&ts, 0, sizeof(ts));
                // "path,tile_w,tile_h,offset_x,offset_y"
                const char* p = val;
                int pi = 0;
                while (*p && *p != ',' && pi < 127) {
                    ts.image_path[pi++] = *p++;
                }
                ts.image_path[pi] = '\0';
                if (*p == ',') ++p;
                sscanf(p, "%d,%d,%d,%d", &ts.tile_w, &ts.tile_h, &ts.offset_x, &ts.offset_y);
                ts.firstgid = 0;
                ts.columns  = 0;
            }
        }
        else if (state == S_LAYER && cur_layer) {
            if      (strcmp(key, "type") == 0) cur_layer->type = LayerTypeOf(val);
            else if (strcmp(key, "data") == 0) { reading_data = true; tile_row = 0; }
        }
        else if (state == S_ENEMY) {
            if (strcmp(key, "category") == 0) {
                strncpy(cur_enemy.category, val, sizeof(cur_enemy.category) - 1);
            } else if (strcmp(key, "location") == 0) {
                int x, y, w, h;
                if (sscanf(val, "%d,%d,%d,%d", &x, &y, &w, &h) == 4) {
                    cur_enemy.center_x = (float)x + (float)w * 0.5f;
                    cur_enemy.center_y = (float)y + (float)h * 0.5f;
                } else if (sscanf(val, "%d,%d", &x, &y) == 2) {
                    cur_enemy.center_x = (float)x;
                    cur_enemy.center_y = (float)y;
                }
            } else if (strcmp(key, "level") == 0) {
                cur_enemy.level = atoi(val);
            } else if (strcmp(key, "number") == 0) {
                int a, b;
                if (sscanf(val, "%d,%d", &a, &b) == 2) cur_enemy.number_min = a;
                else cur_enemy.number_min = atoi(val);
            } else if (strcmp(key, "wander_radius") == 0) {
                cur_enemy.wander_radius = atoi(val);
            }
        }
    }

    // Commit last enemy block.
    if (enemy_open && cur_enemy.category[0] && m.spawn_count < MAX_SPAWNS_PER_MAP) {
        m.spawns[m.spawn_count++] = cur_enemy;
    }

    fclose(f);
    return m.width > 0 && m.height > 0;
}

} // namespace md::flare

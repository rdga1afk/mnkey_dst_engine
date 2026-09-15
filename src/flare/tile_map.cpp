#include <monkey_dust/flare/tile_map.h>
#include "tile_map_internal.h"
#include <cstdio>
#include <cstring>
#include <cstdlib>

namespace md::flare {

// ── TileMetaRegistry ──────────────────────────────────────────────────────────

void TileMetaRegistry::Clear() {
    count = 0;
    atlas_count = 0;
    anim_count = 0;
    atlas_rel_path[0] = '\0';
    for (int i = 0; i < MAX_ATLAS_COUNT; ++i) atlas_paths[i][0] = '\0';
}

const TileMeta* TileMetaRegistry::Find(uint16_t tile_id) const {
    if (tile_id == 0) return nullptr;
    for (int i = 0; i < count; ++i)
        if (entries[i].tile_id == tile_id) return &entries[i];
    return nullptr;
}

bool TileMetaRegistry::AddAnim(const TileAnim& anim) {
    if (anim_count >= MAX_ANIM_TILES) return false;
    for (int i = 0; i < anim_count; ++i) {
        if (anim_entries[i].tile_id == anim.tile_id) { anim_entries[i] = anim; return true; }
    }
    anim_entries[anim_count++] = anim;
    return true;
}

const TileAnim* TileMetaRegistry::FindAnim(uint16_t tile_id) const {
    for (int i = 0; i < anim_count; ++i)
        if (anim_entries[i].tile_id == tile_id) return &anim_entries[i];
    return nullptr;
}

bool TileMetaRegistry::Add(const TileMeta& m) {
    if (count >= MAX_TILE_META) {
        fprintf(stderr, "[TileMeta] registry full (MAX=%d), tile_id=%d dropped\n",
                MAX_TILE_META, (int)m.tile_id);
        return false;
    }
    for (int i = 0; i < count; ++i) {
        if (entries[i].tile_id == m.tile_id) { entries[i] = m; return true; }
    }
    entries[count++] = m;
    return true;
}

void TileMetaRegistry::DumpFirst(int n) const {
    int show = (n < count) ? n : count;
    fprintf(stdout, "[TileMeta] %d entries, %d atlas(es), %d animated (showing first %d):\n",
            count, atlas_count, anim_count, show);
    for (int i = 0; i < show; ++i) {
        const TileMeta& m = entries[i];
        fprintf(stdout, "  [%d] id=%u atlas=%d src=(%d,%d) size=(%d,%d) offset=(%d,%d)\n",
                i, (unsigned)m.tile_id, (int)m.atlas_idx,
                (int)m.src_x, (int)m.src_y,
                (int)m.w, (int)m.h,
                (int)m.offset_x, (int)m.offset_y);
    }
}

// ── Public API ─────────────────────────────────────────────────────────────────

bool LoadFlareMap(const char* path, FlareMap& out) {
    out.meta.Clear();

    const char* dot = strrchr(path, '.');
    if (!dot) return false;

    bool ok = (strcmp(dot, ".tmx") == 0) ? LoadTmx(path, out) : LoadTxt(path, out);
    if (!ok) return false;

    out.tileset_atlas[0] = '\0';
    out.tileset_atlas_count = 0;
    for (int ai = 0; ai < MAX_ATLAS_COUNT; ++ai) out.tileset_atlases[ai][0] = '\0';

    if (out.tileset_def[0]) {
        TryLoadTilesetDef(path, out.tileset_def, out.meta);

        if (out.meta.atlas_count > 0) {
            char map_dir[512], mod_dir[512], mods_root[512];
            DirOf(path, map_dir, sizeof(map_dir));
            DirOf(map_dir, mod_dir, sizeof(mod_dir));
            DirOf(mod_dir, mods_root, sizeof(mods_root));

            // Resolve each atlas section path (M7.23 multi-atlas).
            for (int ai = 0; ai < out.meta.atlas_count && ai < MAX_ATLAS_COUNT; ++ai) {
                char resolved[512];
                if (ResolveAtlasInMods(mods_root, mod_dir,
                                       out.meta.atlas_paths[ai],
                                       resolved, sizeof(resolved))) {
                    strncpy(out.tileset_atlases[ai], resolved,
                            sizeof(out.tileset_atlases[ai]) - 1);
                    fprintf(stdout, "[FlareMap] atlas[%d]: %s\n", ai, resolved);
                    ++out.tileset_atlas_count;
                } else {
                    fprintf(stderr, "[FlareMap] atlas[%d] not found: %s\n",
                            ai, out.meta.atlas_paths[ai]);
                }
            }
            // atlas[0] → backward-compat single-atlas field.
            if (out.tileset_atlases[0][0])
                strncpy(out.tileset_atlas, out.tileset_atlases[0],
                        sizeof(out.tileset_atlas) - 1);
        }
    }

    out.meta.DumpFirst(10);
    return true;
}

bool InitEmptyFlareMap(const char* base_path, int width, int height,
                       const char* tilesetdef, FlareMap& out) {
    memset(&out, 0, sizeof(out));
    if (width  < 1) width  = 1;
    if (height < 1) height = 1;
    if (width  > MAX_MAP_WIDTH)  width  = MAX_MAP_WIDTH;
    if (height > MAX_MAP_HEIGHT) height = MAX_MAP_HEIGHT;

    out.width  = width;
    out.height = height;
    out.tile_w = 192;
    out.tile_h = 96;
    out.hero_x = width  * 0.5f;
    out.hero_y = height * 0.5f;
    snprintf(out.title,       sizeof(out.title),       "new_map");
    snprintf(out.tileset_def, sizeof(out.tileset_def), "%s", tilesetdef ? tilesetdef : "");

    out.layer_count   = 4;
    out.layers[0].type = LayerType::BACKGROUND;
    out.layers[1].type = LayerType::FRINGE;
    out.layers[2].type = LayerType::OBJECT;
    out.layers[3].type = LayerType::COLLISION;

    if (tilesetdef && tilesetdef[0] && base_path && base_path[0]) {
        TryLoadTilesetDef(base_path, tilesetdef, out.meta);

        if (out.meta.atlas_count > 0) {
            char map_dir[512], mod_dir[512], mods_root[512];
            DirOf(base_path, map_dir, sizeof(map_dir));
            DirOf(map_dir,   mod_dir, sizeof(mod_dir));
            DirOf(mod_dir, mods_root, sizeof(mods_root));

            for (int ai = 0; ai < out.meta.atlas_count && ai < MAX_ATLAS_COUNT; ++ai) {
                char resolved[512];
                if (ResolveAtlasInMods(mods_root, mod_dir,
                                       out.meta.atlas_paths[ai],
                                       resolved, sizeof(resolved))) {
                    strncpy(out.tileset_atlases[ai], resolved,
                            sizeof(out.tileset_atlases[ai]) - 1);
                    ++out.tileset_atlas_count;
                }
            }
            if (out.tileset_atlases[0][0])
                strncpy(out.tileset_atlas, out.tileset_atlases[0],
                        sizeof(out.tileset_atlas) - 1);
        }
    }
    return true;
}

// ── Flare .txt serializer ─────────────────────────────────────────────────────

static const char* LayerTypeName(LayerType t) {
    switch (t) {
        case LayerType::BACKGROUND: return "background";
        case LayerType::FRINGE:     return "background_fringe";
        case LayerType::OBJECT:     return "object";
        case LayerType::COLLISION:  return "collision";
        default:                    return "background";
    }
}

bool SaveFlareMap(const char* path, const FlareMap& map) {
    FILE* f = fopen(path, "w");
    if (!f) {
        fprintf(stderr, "[SaveFlareMap] cannot open for write: %s\n", path);
        return false;
    }

    // [header]
    fprintf(f, "[header]\n");
    fprintf(f, "width=%d\n",      map.width);
    fprintf(f, "height=%d\n",     map.height);
    fprintf(f, "tilewidth=%d\n",  map.tile_w);
    fprintf(f, "tileheight=%d\n", map.tile_h);
    if (map.music_path[0])  fprintf(f, "music=%s\n",   map.music_path);
    if (map.tileset_def[0]) fprintf(f, "tileset=%s\n", map.tileset_def);
    if (map.title[0])       fprintf(f, "title=%s\n",   map.title);
    if (map.hero_x != 0.0f || map.hero_y != 0.0f)
        fprintf(f, "hero_pos=%d,%d\n", (int)map.hero_x, (int)map.hero_y);
    fprintf(f, "\n");

    // [tilesets]
    if (map.tileset_count > 0) {
        fprintf(f, "[tilesets]\n");
        for (int i = 0; i < map.tileset_count; ++i) {
            const TileSet& ts = map.tilesets[i];
            if (!ts.image_path[0]) continue;
            fprintf(f, "tileset=%s,%d,%d,%d,%d\n",
                    ts.image_path, ts.tile_w, ts.tile_h,
                    ts.offset_x, ts.offset_y);
        }
        fprintf(f, "\n");
    }

    // [layer] sections — tile data as CSV rows
    for (int li = 0; li < map.layer_count; ++li) {
        const TileMapLayer& layer = map.layers[li];
        if (layer.type == LayerType::UNKNOWN) continue;

        fprintf(f, "[layer]\n");
        fprintf(f, "type=%s\n", LayerTypeName(layer.type));
        fprintf(f, "data\n");
        for (int row = 0; row < map.height; ++row) {
            for (int col = 0; col < map.width; ++col) {
                fprintf(f, "%d", (int)layer.tiles[row * MAX_MAP_WIDTH + col]);
                if (col < map.width - 1) fputc(',', f);
            }
            fputc('\n', f);
        }
        fprintf(f, "\n");
    }

    // [enemy] blocks
    for (int si = 0; si < map.spawn_count; ++si) {
        const FlareSpawn& sp = map.spawns[si];
        if (!sp.category[0]) continue;
        fprintf(f, "[enemy]\n");
        fprintf(f, "category=%s\n", sp.category);
        // Reconstruct a 1×1 spawn rect whose centre matches stored center_x/y.
        // center = x+0.5 → x = (int)center_x (works for half-integer centres too).
        fprintf(f, "location=%d,%d,1,1\n",
                (int)sp.center_x, (int)sp.center_y);
        if (sp.level         > 0) fprintf(f, "level=%d\n",         sp.level);
        if (sp.number_min    > 0) fprintf(f, "number=%d\n",        sp.number_min);
        if (sp.wander_radius > 0) fprintf(f, "wander_radius=%d\n", sp.wander_radius);
        fprintf(f, "\n");
    }

    fclose(f);
    fprintf(stdout, "[SaveFlareMap] wrote %s\n", path);
    return true;
}

bool ResolveTile(const FlareMap& map, uint16_t tile_id,
                 int* out_ts_idx, int* out_local_idx)
{
    if (tile_id == 0 || map.tileset_count == 0) return false;

    // Find the tileset whose firstgid range covers tile_id.
    // For .txt format (firstgid=0 on all tilesets), fall back to index 0.
    int best = -1;
    for (int i = 0; i < map.tileset_count; ++i) {
        int fg = map.tilesets[i].firstgid;
        if (fg == 0) { if (best < 0) best = i; continue; }
        if ((int)tile_id >= fg) {
            if (best < 0 || fg > map.tilesets[best].firstgid) best = i;
        }
    }
    if (best < 0) best = 0;

    *out_ts_idx   = best;
    *out_local_idx = (int)tile_id - map.tilesets[best].firstgid;
    if (*out_local_idx < 0) *out_local_idx = 0;
    return true;
}

} // namespace md::flare

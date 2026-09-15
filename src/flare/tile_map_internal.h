#pragma once
// Shared declarations for tile_map.cpp's format-split translation units
// (tile_map_tilesetdef.cpp, tile_map_txt_io.cpp, tile_map_tmx_io.cpp) --
// Phase 5 split, 2026-09-15. Not a public header (lives in src/, matches
// terrain_gen_internal.h / gpu_hal_buffers_internal.h convention).

#include <monkey_dust/flare/tile_map.h>
#include <cstring>

namespace md::flare {

// Extract parent directory in-place (removes last path component).
// Defined in tile_map_tilesetdef.cpp.
void DirOf(const char* path, char* out, int n);

// Resolve one atlas relative path by searching all sibling mod directories,
// then falling back to the same mod dir. Returns true if found.
// Defined in tile_map_tilesetdef.cpp.
bool ResolveAtlasInMods(const char* mods_root, const char* mod_dir,
                         const char* rel_path, char* out, int out_n);

// Scan siblings of the map's mod directory for a tilesetdef file, parse it.
// Defined in tile_map_tilesetdef.cpp.
bool TryLoadTilesetDef(const char* map_path, const char* tileset_def,
                        TileMetaRegistry& meta);

// Flare-native .txt map format loader. Defined in tile_map_txt_io.cpp.
bool LoadTxt(const char* path, FlareMap& m);

// Tiled .tmx (XML) map format loader. Defined in tile_map_tmx_io.cpp.
bool LoadTmx(const char* path, FlareMap& m);

// Bounded string copy (like strlcpy, not always available). Shared between
// the .txt and .tmx loaders -- small enough to inline.
inline void CpStr(char* dst, int n, const char* src) {
    int i = 0;
    for (; i < n - 1 && src[i]; ++i) dst[i] = src[i];
    dst[i] = '\0';
}

// Map layer-type-name string -> LayerType enum. Shared between the .txt and
// .tmx loaders (different section-name spellings, same enum).
inline LayerType LayerTypeOf(const char* s) {
    if (strcmp(s, "background")       == 0) return LayerType::BACKGROUND;
    if (strcmp(s, "fringe")           == 0) return LayerType::FRINGE;
    if (strcmp(s, "background_fringe")== 0) return LayerType::FRINGE;  // Flare .txt variant
    if (strcmp(s, "backgroundfringe") == 0) return LayerType::FRINGE;  // Tiled TMX variant
    if (strcmp(s, "object")           == 0) return LayerType::OBJECT;
    if (strcmp(s, "collision")        == 0) return LayerType::COLLISION;
    return LayerType::UNKNOWN;
}

} // namespace md::flare

#pragma once
#include <monkey_dust/flare/tile_map.h>
#include <monkey_dust/render/md_camera.h>
#include <monkey_dust/render/md_texture.h>

// Painter's-algorithm sort scratch entry -- fully defined at file scope in
// tile_map_renderer.cpp (not exposed to other TUs). Forward-declared here
// only so TileMapRenderer's private helpers can take pointers to it.
struct VisibleTile;

namespace md::flare {

// Maximum tile instances submitted per Render() call.
// 128×128 map ≈ 16 384 tiles; capped so instance VBO stays fixed-size.
constexpr int MAX_VISIBLE_TILES = 16384;

// ── P14: Tile viewport culling ────────────────────────────────────────────────
// Conservative axis-aligned bounds in tile space.
// col/row ranges are inclusive and pre-clamped to [0, map_w/h−1].
// "Conservative" means: may include a few invisible tiles at isometric edges,
// but guaranteed NEVER to cull tiles that are actually on screen.
struct TileCullBounds {
    int col_min, col_max;   // inclusive column range
    int row_min, row_max;   // inclusive row range
};

// Compute culling bounds given the camera world-position (cam_x, cam_z),
// visible half-extent in world units (ortho_size or FOV-derived estimate),
// tile world size, and map dimensions.
// Returns bounds clamped to [0, map_w-1] × [0, map_h-1].
inline TileCullBounds ComputeTileCullBounds(float cam_x, float cam_z,
                                             float half_ext,
                                             float tile_world_size,
                                             int   map_w, int map_h) noexcept {
    float tsz = (tile_world_size > 0.f) ? tile_world_size : 1.f;
    // Convert half-extent to tile units; add 2-tile isometric skew margin.
    float ext = half_ext / tsz + 2.f;
    float cc = cam_x / tsz;
    float cr = cam_z / tsz;
    TileCullBounds b;
    b.col_min = (int)(cc - ext);
    b.col_max = (int)(cc + ext) + 1;
    b.row_min = (int)(cr - ext);
    b.row_max = (int)(cr + ext) + 1;
    if (b.col_min < 0)       b.col_min = 0;
    if (b.col_max < 0)       b.col_max = 0;
    if (b.col_max >= map_w)  b.col_max = map_w - 1;
    if (b.row_min < 0)       b.row_min = 0;
    if (b.row_max < 0)       b.row_max = 0;
    if (b.row_max >= map_h)  b.row_max = map_h - 1;
    return b;
}

// Renders one FlareMap background + object layers as instanced geometry.
// Supports multiple atlas textures (M7.23): tiles from different [tileset]
// sections in the tilesetdef are batched by atlas and drawn with texture switches
// while maintaining global painter's depth order (col+row sort).
//
// UV CONVENTION: atlases loaded via MdLoadTexturePixelArt (stbi flip=1).
//   v_gl = 1.0f − y_file/atlas_h  ← MUST match tile_map_renderer.cpp
//   Flat tile  (h≤96): v0=1−src_y/H (north), v1=1−(src_y+h)/H (south)
//   Billboard  (h>96): v0=1−(src_y+h)/H (base), v1=1−src_y/H (tip)
//
// Per-frame usage:
//   SetAtlases(map)                     — once after LoadFlareMap()
//   Render(map, cam, aspect, tile_size) — one or more instanced draws per frame
class TileMapRenderer {
public:
    static TileMapRenderer& Get();

    void Init();
    void Shutdown();

    // Load all atlas textures from a parsed FlareMap.
    // Call once after LoadFlareMap(); replaces any previously loaded atlases.
    void SetAtlases(const FlareMap& map);

    // Single-atlas convenience — loads one image as atlas[0].
    // Kept for backward compat; prefer SetAtlases() for multi-atlas maps.
    void SetAtlas(const char* png_path);

    // Render map background + object layers in painter's depth order.
    // tile_world_size: world-space footprint of one tile (default 1.0).
    // ortho_size:      orthographic half-height in world units (0 = perspective).
    // now_s:           elapsed time in seconds for tile animation (GetTime()).
    void Render(const FlareMap& map, const MdCamera& cam,
                float aspect, float tile_world_size = 1.0f,
                float ortho_size = 0.0f, float now_s = 0.0f);

    // Invalidate the cached TINST buffer — call after editing map tiles at runtime.
    void MarkDirty() noexcept { dirty_ = true; }

private:
    TileMapRenderer() = default;

    // Render() split (Phase 5, 2026-09-15): PASS 1+2 (collect+sort) and
    // PASS 3 (UV/Y-extent instance-data build) factored out of the dirty-
    // rebuild block. vbuf/ibuf/n are the same scratch state Render() always
    // used locally -- passed by pointer/reference here, no new persistent
    // state, same per-call scratch lifetime.
    void CollectAndSortVisibleTiles(const FlareMap& map, VisibleTile* vbuf, int& n) const;
    void BuildInstanceData(const FlareMap& map, const VisibleTile* vbuf, int n,
                            float now_s, float tile_world_size, uint8_t* ibuf) const;

    uint32_t vao_      = 0;
    uint32_t quad_vbo_ = 0;
    uint32_t inst_vbo_ = 0;
    uint32_t prog_     = 0;

    int loc_vp_        = -1;
    int loc_tile_size_ = -1;
    int loc_y_         = -1;

    MdTexture        atlases_[MAX_ATLAS_COUNT];
    int              atlas_count_ = 0;
    bool             init_        = false;
    bool             dirty_       = true;   // true → rebuild TINST cache next Render()
    const FlareMap*  last_map_    = nullptr;
};

} // namespace md::flare

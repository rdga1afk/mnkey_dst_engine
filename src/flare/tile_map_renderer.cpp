#include <monkey_dust/flare/tile_map_renderer.h>
#include <monkey_dust/render/md_shader.h>

#include "glad.h"
#include <monkey_dust/platform/math_types.h>
#include <monkey_dust/platform/md_log.h>
#include <cstring>
#include <cstdio>
#include <cstdlib>    // qsort

// Per-instance GPU layout (stride = 36 bytes — M7.27 horizontal offset):
//   vec2  tile_pos  (grid col, row)   offset  0
//   vec4  uv_rect   (u0,v0,u1,v1)    offset  8
//   float y_bot     world Y of base  offset 24  (≤0 or 0 for flat tile)
//   float y_top     world Y of tip   offset 28  (>0 = billboard; 0 = flat tile)
//   float x_off     screen-horiz     offset 32  (world units; +→right on screen)
//
// BILLBOARD CLASSIFICATION: is_billboard = (offset_y > h / 2)
//   Anchor in lower half → billboard (trees/mushrooms).
//   Anchor in upper half (offset_y ≤ h/2) → flat XZ diamond (water, cliff, grass).
//   NOTE: offset_y == h/2 (wall tiles 16-47, h=96) currently classified as flat.
//   Pending ground-truth comparison with original Flare rendering before changing to >=.
// Y FORMULA (96 atlas-px = 1 world unit):
//   y_top = offset_y / 96.0 * tsz,  y_bot = -(h - offset_y) / 96.0 * tsz
//
// HORIZONTAL OFFSET (M7.27):
//   In Flare: sprite_screen_x_left = grid_x - offset_x.
//   Center deviation from grid: dx_px = w/2 - offset_x  (positive = right on screen).
//   Screen-right in world = (+Δwx, 0, +Δwz) where 96 screen-px = tsz/2 world units.
//   x_off = (w/2 - offset_x) * tile_world_size / 192.0
static constexpr int TINST_STRIDE    = 36;
static constexpr int TINST_OFF_POS   =  0;
static constexpr int TINST_OFF_UV    =  8;
static constexpr int TINST_OFF_YBOT  = 24;
static constexpr int TINST_OFF_YTOP  = 28;
static constexpr int TINST_OFF_XOFF  = 32;

// Unit quad corners [0,1]×[0,1] (4 verts, GL_TRIANGLE_FAN).
static const float TILE_QUAD[8] = {
    0.f, 0.f,  1.f, 0.f,  1.f, 1.f,  0.f, 1.f
};

// Painter's algorithm sort scratch.
// M10: depth is float to allow sub-tile ordering corrections.
struct VisibleTile {
    uint16_t col;
    uint16_t row;
    uint16_t tid;
    uint8_t  atlas_idx;  // TileMeta::atlas_idx — which atlas texture to sample
    uint8_t  _pad;
    int      local_idx;  // tileset-local UV index (grid fallback only)
    float    depth;      // (col+row)*3 + layer_prio + x_off_correction
};
static int VisibleTileCmp(const void* a, const void* b) {
    float da = ((const VisibleTile*)a)->depth;
    float db = ((const VisibleTile*)b)->depth;
    return (da > db) - (da < db);
}

namespace md::flare {

TileMapRenderer& TileMapRenderer::Get() {
    static TileMapRenderer inst;
    return inst;
}

void TileMapRenderer::Init() {
    if (init_) return;

    glGenBuffers(1, &quad_vbo_);
    glBindBuffer(GL_ARRAY_BUFFER, quad_vbo_);
    glBufferData(GL_ARRAY_BUFFER, sizeof(TILE_QUAD), TILE_QUAD, GL_STATIC_DRAW);

    glGenBuffers(1, &inst_vbo_);
    glBindBuffer(GL_ARRAY_BUFFER, inst_vbo_);
    glBufferData(GL_ARRAY_BUFFER,
                 (GLsizeiptr)(MAX_VISIBLE_TILES * TINST_STRIDE), nullptr, GL_STREAM_DRAW);

    glGenVertexArrays(1, &vao_);
    glBindVertexArray(vao_);

    // Attrib 0: quad corner (vec2, per-vertex)
    glBindBuffer(GL_ARRAY_BUFFER, quad_vbo_);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 8, (void*)0);
    glVertexAttribDivisor(0, 0);

    // Attribs 1-2: per-instance data
    glBindBuffer(GL_ARRAY_BUFFER, inst_vbo_);

    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, TINST_STRIDE, (void*)TINST_OFF_POS);
    glVertexAttribDivisor(1, 1);

    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, TINST_STRIDE, (void*)TINST_OFF_UV);
    glVertexAttribDivisor(2, 1);

    // attrib 3: y_bot (1 float at offset 24)
    glEnableVertexAttribArray(3);
    glVertexAttribPointer(3, 1, GL_FLOAT, GL_FALSE, TINST_STRIDE, (void*)(intptr_t)TINST_OFF_YBOT);
    glVertexAttribDivisor(3, 1);
    // attrib 4: y_top (1 float at offset 28)
    glEnableVertexAttribArray(4);
    glVertexAttribPointer(4, 1, GL_FLOAT, GL_FALSE, TINST_STRIDE, (void*)(intptr_t)TINST_OFF_YTOP);
    glVertexAttribDivisor(4, 1);
    // attrib 5: x_off (1 float at offset 32) — M7.27 horizontal sprite offset
    glEnableVertexAttribArray(5);
    glVertexAttribPointer(5, 1, GL_FLOAT, GL_FALSE, TINST_STRIDE, (void*)(intptr_t)TINST_OFF_XOFF);
    glVertexAttribDivisor(5, 1);

    glBindVertexArray(0);

    MdShader sh = MdLoadShader("shaders/tile_map.vert", "shaders/tile_map.frag");
    prog_ = sh.id;
    if (prog_) {
        loc_vp_        = MdGetLoc(sh, "u_vp");
        loc_tile_size_ = MdGetLoc(sh, "u_tile_size");
        loc_y_         = MdGetLoc(sh, "u_y");
    } else {
        fprintf(stderr, "[TileMap] failed to load tile_map shaders\n");
    }

    init_ = true;
}

void TileMapRenderer::Shutdown() {
    if (!init_) return;
    for (int i = 0; i < atlas_count_; ++i) MdUnloadTexture(atlases_[i]);
    atlas_count_ = 0;
    glDeleteVertexArrays(1, &vao_);
    glDeleteBuffers(1, &quad_vbo_);
    glDeleteBuffers(1, &inst_vbo_);
    if (prog_) { glDeleteProgram(prog_); prog_ = 0; }
    init_ = false;
}

void TileMapRenderer::SetAtlases(const FlareMap& map) {
    dirty_ = true;
    for (int i = 0; i < atlas_count_; ++i) MdUnloadTexture(atlases_[i]);
    atlas_count_ = 0;
    for (int i = 0; i < map.tileset_atlas_count && i < MAX_ATLAS_COUNT; ++i) {
        if (!map.tileset_atlases[i][0]) { ++atlas_count_; continue; }
        atlases_[atlas_count_] = MdLoadTexturePixelArt(map.tileset_atlases[i]);
        if (!atlases_[atlas_count_].id)
            fprintf(stderr, "[TileMap] atlas[%d] failed: %s\n", i, map.tileset_atlases[i]);
        ++atlas_count_;
    }
}

void TileMapRenderer::SetAtlas(const char* png_path) {
    dirty_ = true;
    for (int i = 0; i < atlas_count_; ++i) MdUnloadTexture(atlases_[i]);
    atlas_count_ = 1;
    atlases_[0] = MdLoadTexturePixelArt(png_path);
    if (!atlases_[0].id)
        fprintf(stderr, "[TileMap] failed to load atlas: %s\n", png_path);
}

// PASS 1: collect layers into vbuf, PASS 2: sort back-to-front (ascending
// col+row = painter's order). layer_prio encodes render order within the
// same (col+row) depth bucket: 0=BACKGROUND (ground), 1=FRINGE (ground-level
// overlays), 2=OBJECT (tall). depth = (col+row)*3 + layer_prio ensures
// bg < fringe < object at equal depth.
void TileMapRenderer::CollectAndSortVisibleTiles(const FlareMap& map, VisibleTile* vbuf, int& n) const {
    // Pick background layer (first BACKGROUND, else layer 0).
    const TileMapLayer* bg = nullptr;
    for (int i = 0; i < map.layer_count; ++i)
        if (map.layers[i].type == LayerType::BACKGROUND) { bg = &map.layers[i]; break; }
    if (!bg) bg = &map.layers[0];

    // Detect .txt format: all tilesets have firstgid==0.
    bool txt_fmt = true;
    for (int i = 0; i < map.tileset_count; ++i)
        if (map.tilesets[i].firstgid > 0) { txt_fmt = false; break; }

    // Visual tileset for grid-based UV fallback (atlas[0] only) -- only
    // vis_max is needed here (local_idx bounds check); the rest of the
    // visual-tileset grid math is BuildInstanceData's concern.
    int vis_ts_idx = (txt_fmt && map.tileset_count > 1) ? 1 : 0;
    if (!txt_fmt) {
        for (int i = 0; i < map.tileset_count; ++i)
            if (map.tilesets[i].firstgid >= 2) { vis_ts_idx = i; break; }
    }
    const TileSet& vis  = map.tilesets[vis_ts_idx];
    const MdTexture& a0 = atlases_[0];
    int vis_cols = vis.columns > 0 ? vis.columns :
                   (a0.w > 0 && vis.tile_w > 0) ? a0.w / vis.tile_w : 16;
    if (vis_cols < 1) vis_cols = 1;
    int vis_rows = (a0.h > 0 && vis.tile_h > 0) ? a0.h / vis.tile_h : 8;
    int vis_max  = vis_cols * vis_rows;

    n = 0;
    auto CollectLayer = [&](const TileMapLayer& layer, int layer_prio) {
        for (int row = 0; row < map.height && row < MAX_MAP_HEIGHT && n < MAX_VISIBLE_TILES; ++row) {
            for (int col = 0; col < map.width && col < MAX_MAP_WIDTH && n < MAX_VISIBLE_TILES; ++col) {
                uint16_t tid = layer.tiles[row * MAX_MAP_WIDTH + col];
                if (tid == 0) continue;

                int local_idx;
                if (txt_fmt) {
                    local_idx = (int)tid - 1;
                } else {
                    int ts_idx = 0;
                    if (!ResolveTile(map, tid, &ts_idx, &local_idx)) continue;
                    if (map.tilesets[ts_idx].firstgid < 2) continue;
                }

                const TileMeta* tm_c = map.meta.Find(tid);
                uint8_t aidx = tm_c ? tm_c->atlas_idx : 0;
                if (!tm_c && (local_idx < 0 || local_idx >= vis_max)) continue;

                // M10: float depth with sub-tile x_off correction.
                // x_off_corr = (w/2 - offset_x) / 192 shifts sprite's effective
                // sort position toward its visual center (screen-right = +depth).
                // Clamped to ±0.9 so it never crosses a layer_prio boundary (1.0).
                float depth_f = (float)((col + row) * 3 + layer_prio);
                if (tm_c && tm_c->w > 0) {
                    float x_corr = ((float)tm_c->w * 0.5f - (float)tm_c->offset_x)
                                   / 192.0f;
                    if (x_corr >  0.9f) x_corr =  0.9f;
                    if (x_corr < -0.9f) x_corr = -0.9f;
                    depth_f += x_corr;
                }
                vbuf[n].col       = (uint16_t)col;
                vbuf[n].row       = (uint16_t)row;
                vbuf[n].tid       = tid;
                vbuf[n].atlas_idx = aidx;
                vbuf[n].local_idx = local_idx;
                vbuf[n].depth     = depth_f;
                ++n;
            }
        }
    };

    CollectLayer(*bg, 0);
    for (int i = 0; i < map.layer_count; ++i)
        if (map.layers[i].type == LayerType::FRINGE)
            CollectLayer(map.layers[i], 1);
    for (int i = 0; i < map.layer_count; ++i)
        if (map.layers[i].type == LayerType::OBJECT)
            CollectLayer(map.layers[i], 2);

    qsort(vbuf, (size_t)n, sizeof(VisibleTile), VisibleTileCmp);
}

// PASS 3: compute UV and world Y extents for every tile into ibuf.
//
// UV CONVENTION (stbi flip active — see tile_map.h):
//   Flat:      v0 = 1-(src_y/H)      [north/top],   v1 = 1-((src_y+h)/H) [south/bottom]
//   Billboard: v0 = 1-((src_y+h)/H)  [base/anchor], v1 = 1-(src_y/H)     [tip/crown]
//
// BILLBOARD CLASSIFICATION (M7.24):
//   is_billboard = (TileMeta::offset_y > TileMeta::h / 2)
//   Anchor in lower half → billboard. Upper half (offset_y ≤ h/2) → flat XZ diamond.
//
// Y EXTENTS for billboards (96 atlas-px = 1 world unit):
//   y_top = offset_y / 96 * tsz           (tip, above ground)
//   y_bot = -(h - offset_y) / 96 * tsz    (root, clipped below ground by depth test)
void TileMapRenderer::BuildInstanceData(const FlareMap& map, const VisibleTile* vbuf, int n,
                                         float now_s, float tile_world_size, uint8_t* ibuf) const {
    // Visual tileset grid params for the no-metadata fallback branch below
    // (recomputed here rather than threaded from CollectAndSortVisibleTiles --
    // cheap, and keeps the two passes independently callable/testable).
    bool txt_fmt = true;
    for (int i = 0; i < map.tileset_count; ++i)
        if (map.tilesets[i].firstgid > 0) { txt_fmt = false; break; }
    int vis_ts_idx = (txt_fmt && map.tileset_count > 1) ? 1 : 0;
    if (!txt_fmt) {
        for (int i = 0; i < map.tileset_count; ++i)
            if (map.tilesets[i].firstgid >= 2) { vis_ts_idx = i; break; }
    }
    const TileSet& vis  = map.tilesets[vis_ts_idx];
    const MdTexture& a0 = atlases_[0];
    int vis_cols = vis.columns > 0 ? vis.columns :
                   (a0.w > 0 && vis.tile_w > 0) ? a0.w / vis.tile_w : 16;
    if (vis_cols < 1) vis_cols = 1;
    float iaw = (a0.w > 0 && vis.tile_w > 0) ? (float)vis.tile_w / a0.w : 1.0f / vis_cols;
    float iah = (a0.h > 0 && vis.tile_h > 0) ? (float)vis.tile_h / a0.h : iaw;
    float ground_iah = (a0.h > 0 && map.tile_h > 0) ? (float)map.tile_h / (float)a0.h : iah;

    for (int i = 0; i < n; ++i) {
        const VisibleTile& vt = vbuf[i];
        float u0, v0, u1, v1;
        float y_bot = 0.0f, y_top = 0.0f;  // 0/0 = flat tile sentinel

        const TileMeta* tm = map.meta.Find(vt.tid);

        // Animation: pick the current frame's src_x/src_y if this tile animates.
        int16_t frame_sx = tm ? tm->src_x : 0;
        int16_t frame_sy = tm ? tm->src_y : 0;
        if (tm && now_s > 0.0f) {
            const TileAnim* anim = map.meta.FindAnim(vt.tid);
            if (anim && anim->total_ms > 0) {
                uint32_t t = (uint32_t)(now_s * 1000.0f) % anim->total_ms;
                uint32_t accum = 0;
                for (int fi = 0; fi < anim->frame_count; ++fi) {
                    accum += anim->frames[fi].duration_ms;
                    if (t < accum) {
                        frame_sx = anim->frames[fi].src_x;
                        frame_sy = anim->frames[fi].src_y;
                        break;
                    }
                }
            }
        }

        if (tm && tm->w > 0 && tm->h > 0) {
            uint8_t aidx = tm->atlas_idx < (uint8_t)atlas_count_ ? tm->atlas_idx : 0;
            const MdTexture& atl = atlases_[aidx];
            u0 = (float)frame_sx / (float)atl.w;
            u1 = (float)(frame_sx + tm->w) / (float)atl.w;

            // Billboard: anchor row in lower half of sprite (offset_y > h/2).
            // Flat: anchor in upper half or center (offset_y ≤ h/2) → XZ diamond.
            bool is_bb = (tm->offset_y > tm->h / 2);
            if (is_bb) {
                // UV: corner.y=0=base(bottom of image), corner.y=1=tip(top of image).
                v0 = 1.0f - (float)(frame_sy + tm->h) / (float)atl.h;
                v1 = 1.0f - (float)frame_sy / (float)atl.h;
                // World Y: 96 atlas-px → 1 world unit.
                y_top = (float)tm->offset_y / 96.0f * tile_world_size;
                y_bot = -((float)tm->h - (float)tm->offset_y) / 96.0f * tile_world_size;
            } else {
                // UV: corner.y=0=north(top of image), corner.y=1=south(bottom).
                v0 = 1.0f - (float)frame_sy / (float)atl.h;
                v1 = 1.0f - (float)(frame_sy + tm->h) / (float)atl.h;
                // y_bot = y_top = 0 → flat tile (shader uses XZ diamond).
            }
        } else {
            // Grid fallback (no metadata — uses atlas[0] grid layout).
            float tile_h_f = 96.0f;
            if (tm && tm->h > 0) tile_h_f = (float)tm->h;
            int tc = vt.local_idx % vis_cols;
            int tr = vt.local_idx / vis_cols;
            u0 = tc * iaw;
            u1 = u0 + iaw;
            if (tile_h_f <= 96.5f) {
                // Flat diamond.
                v1 = 1.0f - (float)(tr + 1) * iah;
                v0 = v1 + ground_iah;
                // y_bot = y_top = 0 already.
            } else {
                // Tall grid tile: assume standard anchor (billboard rising from y=0).
                v0 = 1.0f - (float)(tr + 1) * iah;
                v1 = 1.0f - (float)tr * iah;
                y_top = (tile_h_f - 96.0f) / 96.0f * tile_world_size;
                // y_bot = 0: no below-ground portion assumed for grid fallback.
            }
        }

        // M7.27: horizontal sprite offset in world units (screen-right = +wx, +wz).
        float x_off = 0.0f;
        if (tm) x_off = ((float)tm->w * 0.5f - (float)tm->offset_x) * (tile_world_size / 192.0f);

        float tx   = (float)vt.col;
        float ty_f = (float)vt.row;
        uint8_t* p = ibuf + i * TINST_STRIDE;
        memcpy(p + TINST_OFF_POS + 0,  &tx,    4);
        memcpy(p + TINST_OFF_POS + 4,  &ty_f,  4);
        memcpy(p + TINST_OFF_UV  + 0,  &u0,    4);
        memcpy(p + TINST_OFF_UV  + 4,  &v0,    4);
        memcpy(p + TINST_OFF_UV  + 8,  &u1,    4);
        memcpy(p + TINST_OFF_UV  + 12, &v1,    4);
        memcpy(p + TINST_OFF_YBOT,     &y_bot, 4);
        memcpy(p + TINST_OFF_YTOP,     &y_top, 4);
        memcpy(p + TINST_OFF_XOFF,     &x_off, 4);
    }
}

void TileMapRenderer::Render(const FlareMap& map, const MdCamera& cam,
                              float aspect, float tile_world_size, float ortho_size, float now_s)
{
    if (!init_ || !prog_) return;
    if (atlas_count_ == 0) {
        static bool w = false;
        if (!w) { MD_LOG(MD_LOG_WARNING, "[TileMap] Render() called with no atlases"); w = true; }
        return;
    }
    if (map.layer_count == 0 || map.width <= 0 || map.height <= 0) return;

    static VisibleTile vbuf[MAX_VISIBLE_TILES];
    static uint8_t     ibuf[MAX_VISIBLE_TILES * TINST_STRIDE];
    static int     n_cached   = 0;
    static float   last_now_s = -1.0f;

    if (last_map_ != &map) { dirty_ = true; last_map_ = &map; }

    bool has_anims    = (map.meta.anim_count > 0);
    bool anim_changed = has_anims && (now_s != last_now_s);
    int  n            = n_cached;

    if (dirty_ || anim_changed) {
        n = 0;
        dirty_     = false;
        last_now_s = now_s;
        CollectAndSortVisibleTiles(map, vbuf, n);
        BuildInstanceData(map, vbuf, n, now_s, tile_world_size, ibuf);
        n_cached = n;
    }

    if (n == 0) return;

    // GL state for isometric tile rendering.
    glDisable(GL_CULL_FACE);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);
    glDepthMask(GL_TRUE);
    glUseProgram(prog_);

    Mat4 V = cam.ViewMatrix();
    Mat4 P;
    if (ortho_size > 0.0f) {
        const float oh = ortho_size, ow = oh * aspect;
        P = mat4_ortho(-ow, +ow, -oh, +oh, 0.1f, 300.0f);
    } else {
        P = cam.ProjMatrix(aspect);
    }
    Mat4 vp = mat4_mul(V, P);
    // Raylib Matrix struct memory is row-grouped ([m0,m4,m8,m12,...]),
    // not OpenGL column-major ([m0,m1,m2,m3,...]). GL_TRUE tells OpenGL
    // to transpose on load — equivalent to the old MatrixToFloat()+GL_FALSE path.
    // GLM's value_ptr() is already column-major → GL_FALSE is correct there.
#ifdef USE_GLM
    glUniformMatrix4fv(loc_vp_, 1, GL_FALSE, mat4_ptr(vp));
#else
    glUniformMatrix4fv(loc_vp_, 1, GL_TRUE,  mat4_ptr(vp));
#endif

    float tsz[2] = { tile_world_size, tile_world_size };
    glUniform2fv(loc_tile_size_, 1, tsz);
    glUniform1f (loc_y_, 0.0f);

    glBindVertexArray(vao_);
    glBindBuffer(GL_ARRAY_BUFFER, inst_vbo_);

    // P14: compute camera-space culling bounds for this frame.
    // half_ext = ortho half-height when ortho; FOV-based estimate for perspective.
    float half_ext;
    if (ortho_size > 0.f) {
        half_ext = ortho_size;
    } else {
        // Perspective: approximate visible ground radius as camera height × fov factor.
        float ground_h = (cam.pos.y > 0.f) ? cam.pos.y : 20.f;
        half_ext = ground_h * 1.5f;  // conservative for typical isometric FOV
    }
    const TileCullBounds cull = ComputeTileCullBounds(
        cam.pos.x, cam.pos.z, half_ext, tile_world_size, map.width, map.height);

    // PASS 4: draw contiguous atlas runs in painter's depth order.
    // Tiles outside the camera's tile-space culling bounds are skipped.
    // Visible tiles are compacted into cull_ibuf before upload.
    {
        static uint8_t cull_ibuf[MAX_VISIBLE_TILES * TINST_STRIDE];
        int i = 0;
        while (i < n) {
            uint8_t aidx        = vbuf[i].atlas_idx;
            int     batch_start = i;
            while (i < n && vbuf[i].atlas_idx == aidx) ++i;

            // Compact: only copy tiles inside cull bounds into cull_ibuf.
            int vis = 0;
            for (int j = batch_start; j < i; ++j) {
                int vc = (int)vbuf[j].col, vr = (int)vbuf[j].row;
                if (vc >= cull.col_min && vc <= cull.col_max &&
                    vr >= cull.row_min && vr <= cull.row_max) {
                    memcpy(cull_ibuf + vis * TINST_STRIDE,
                           ibuf + j * TINST_STRIDE, TINST_STRIDE);
                    ++vis;
                }
            }
            if (vis == 0) continue;

            glBufferSubData(GL_ARRAY_BUFFER, 0,
                            (GLsizeiptr)(vis * TINST_STRIDE), cull_ibuf);

            if (aidx < (uint8_t)atlas_count_ && atlases_[aidx].id)
                MdBindTexture(atlases_[aidx], 0);

            glDrawArraysInstanced(GL_TRIANGLE_FAN, 0, 4, vis);
        }
    }

    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindVertexArray(0);
    glEnable(GL_CULL_FACE);
    glUseProgram(0);
}

} // namespace md::flare

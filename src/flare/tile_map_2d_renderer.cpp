#include <monkey_dust/flare/tile_map_2d_renderer.h>
#include <monkey_dust/render/md_shader.h>

#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <cmath>

#include "glad.h"

#ifdef MD_SDL_GPU
#include <monkey_dust/render/gpu_device.h>
#include <monkey_dust/render/gpu_hal_types.h>
#include <monkey_dust/render/gpu_pipeline.h>
#include <monkey_dust/render/gpu_copy_pass.h>
#include <monkey_dust/render/gpu_sampler_texture.h>
#include <monkey_dust/render/gpu_hal_free_functions.h>
#endif

// ── Shared helpers ────────────────────────────────────────────────────────────

static constexpr int TILE_W_HALF = 96;
static constexpr int TILE_H_HALF = 48;

struct Tile2D {
    int      col, row, tid, layer_prio;
    uint64_t prio;  // FL-1: Flare-inspired 64-bit key; eliminates Z-fight at tile edges
};

// prio = (col+row)<<37 | col<<20 | layer_prio<<8
// Bits 63-37: diagonal depth (primary), 36-20: col tiebreak, 19-8: layer, 7-0: reserved
static int Tile2DCmp(const void* a, const void* b) {
    uint64_t pa = ((const Tile2D*)a)->prio;
    uint64_t pb = ((const Tile2D*)b)->prio;
    return (pa > pb) - (pa < pb);
}

static uint64_t Tile2DPrio(int col, int row, int layer_prio) {
    return ((uint64_t)(col + row) << 37) |
           ((uint64_t)col         << 20) |
           ((uint64_t)layer_prio  <<  8);
}

static int LayerPrio2D(md::flare::LayerType t) {
    using LT = md::flare::LayerType;
    switch (t) {
        case LT::BACKGROUND: return 0;
        case LT::FRINGE:     return 1;
        case LT::OBJECT:     return 2;
        default:             return 1;
    }
}

namespace md::flare {

TileMap2DRenderer& TileMap2DRenderer::Get() {
    static TileMap2DRenderer inst;
    return inst;
}

// ── Init ──────────────────────────────────────────────────────────────────────

void TileMap2DRenderer::Init() {
    if (init_) return;

#ifdef MD_SDL_GPU
    if (md::GpuDevice::Get().IsReady()) {
        GpuPipeline::Desc pd;
        pd.vert_path           = "shaders/tile_map_2d.vert";
        pd.frag_path           = "shaders/tile_map_2d.frag";
        pd.raster.topology     = GpuTopology::TRIANGLES;
        pd.raster.blend_enable = true;
        pd.raster.src_factor   = GpuBlendFactor::SRC_ALPHA;
        pd.raster.dst_factor   = GpuBlendFactor::ONE_MINUS_SRC_ALPHA;
        pd.raster.depth_test   = false;
        pd.raster.depth_write  = false;
        pd.raster.cull_back    = false;
        pd.vert_uniform_bufs   = 1;  // set=1,binding=0: {vec2 viewport, float alpha_mod, float pad}
        pd.frag_samplers       = 4;  // set=2,binding=0..3: atlases
        pd.has_depth_target    = false;
        pd.layout.stride       = (uint32_t)STRIDE_SDL;
        pd.layout.count        = 5;
        pd.layout.attribs[0]   = {0,  0, GpuAttribFmt::F2};  // a_corner
        pd.layout.attribs[1]   = {1,  8, GpuAttribFmt::F2};  // a_screen_tl
        pd.layout.attribs[2]   = {2, 16, GpuAttribFmt::F2};  // a_screen_size
        pd.layout.attribs[3]   = {3, 24, GpuAttribFmt::F4};  // a_uv_rect
        pd.layout.attribs[4]   = {4, 40, GpuAttribFmt::F1};  // a_atlas_idx

        if (!sdl_pipeline_.Create(pd))
            fprintf(stderr, "[TileMap2D] SDL_GPU: pipeline create failed\n");

        sdl_vbuf_.Init((uint32_t)MAX_TILES * 6u, (uint32_t)STRIDE_SDL);

        // 1×1 opaque white dummy: transparent would be discarded (c.a<0.1) by the
        // tile shader, and NPC dots (atlas_idx=atlas_count_) rely on this slot.
        GpuSamplerDesc ds;
        ds.min_filter = GpuSamplerDesc::Filter::NEAREST;
        ds.mag_filter = GpuSamplerDesc::Filter::NEAREST;
        ds.wrap_s     = GpuSamplerDesc::Wrap::CLAMP_TO_EDGE;
        ds.wrap_t     = GpuSamplerDesc::Wrap::CLAMP_TO_EDGE;
        ds.gen_mipmap = false;
        ds.flip_v     = false;
        uint8_t pix[4] = {255, 255, 255, 255};
        GpuTexture dgt;
        if (dgt.InitFromMemory(pix, 1, 1, ds)) {
            sdl_dummy_tex_     = dgt.TakeSDLTexture();
            sdl_dummy_sampler_ = dgt.TakeSDLSampler();
        }
        sdl_init_ = true;
        init_     = true;
        return;
    }
#endif

    {
        static const float QUAD[8] = { 0,0, 1,0, 1,1, 0,1 };
        glGenBuffers(1, &quad_vbo_);
        glBindBuffer(GL_ARRAY_BUFFER, quad_vbo_);
        glBufferData(GL_ARRAY_BUFFER, sizeof(QUAD), QUAD, GL_STATIC_DRAW);

        glGenBuffers(1, &inst_vbo_);
        glBindBuffer(GL_ARRAY_BUFFER, inst_vbo_);
        glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(MAX_TILES * STRIDE), nullptr, GL_STREAM_DRAW);

        glGenVertexArrays(1, &vao_);
        glBindVertexArray(vao_);

        glBindBuffer(GL_ARRAY_BUFFER, quad_vbo_);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 8, (void*)0);
        glVertexAttribDivisor(0, 0);

        glBindBuffer(GL_ARRAY_BUFFER, inst_vbo_);
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, STRIDE, (void*)0);
        glVertexAttribDivisor(1, 1);
        glEnableVertexAttribArray(2);
        glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, STRIDE, (void*)8);
        glVertexAttribDivisor(2, 1);
        glEnableVertexAttribArray(3);
        glVertexAttribPointer(3, 4, GL_FLOAT, GL_FALSE, STRIDE, (void*)16);
        glVertexAttribDivisor(3, 1);
        glEnableVertexAttribArray(4);
        glVertexAttribPointer(4, 1, GL_FLOAT, GL_FALSE, STRIDE, (void*)(intptr_t)32);
        glVertexAttribDivisor(4, 1);
        glBindVertexArray(0);

        MdShader sh = MdLoadShader("shaders/tile_map_2d.vert", "shaders/tile_map_2d.frag");
        prog_ = sh.id;
        if (prog_) {
            loc_viewport_ = MdGetLoc(sh, "u_viewport");
            loc_atlas_[0] = MdGetLoc(sh, "u_atlas0");
            loc_atlas_[1] = MdGetLoc(sh, "u_atlas1");
            loc_atlas_[2] = MdGetLoc(sh, "u_atlas2");
            loc_atlas_[3] = MdGetLoc(sh, "u_atlas3");
        } else {
            fprintf(stderr, "[TileMap2D] GL: failed to load shaders\n");
        }
    }
    init_ = true;
}

// ── Shutdown ──────────────────────────────────────────────────────────────────

void TileMap2DRenderer::Shutdown() {
    if (!init_) return;
    for (int i = 0; i < atlas_count_; ++i) MdUnloadTexture(atlases_[i]);
    atlas_count_ = 0;

#ifdef MD_SDL_GPU
    if (sdl_init_) {
        sdl_pipeline_.Destroy();
        sdl_vbuf_.Shutdown();
        md::GpuDeviceHandle dev = md::GpuDevice::Get().SDLDevice();
        if (dev) {
            if (sdl_dummy_sampler_) GpuReleaseSampler(dev, (SDL_GPUSampler*)sdl_dummy_sampler_);
            if (sdl_dummy_tex_)     GpuReleaseTexture(dev, (md::GpuTextureHandle)sdl_dummy_tex_);
        }
        sdl_dummy_tex_ = sdl_dummy_sampler_ = nullptr;
        sdl_init_ = false;
    }
#endif

    if (vao_)      { glDeleteVertexArrays(1, &vao_);      vao_      = 0; }
    if (quad_vbo_) { glDeleteBuffers(1, &quad_vbo_);      quad_vbo_ = 0; }
    if (inst_vbo_) { glDeleteBuffers(1, &inst_vbo_);      inst_vbo_ = 0; }
    if (prog_)     { glDeleteProgram(prog_);               prog_     = 0; }

    init_ = false;
}

// ── SetAtlases ────────────────────────────────────────────────────────────────

void TileMap2DRenderer::SetAtlases(const FlareMap& map) {
    for (int i = 0; i < atlas_count_; ++i) MdUnloadTexture(atlases_[i]);
    atlas_count_ = 0;
    for (int i = 0; i < map.tileset_atlas_count && i < MAX_ATLAS; ++i) {
        if (!map.tileset_atlases[i][0]) { ++atlas_count_; continue; }
        atlases_[atlas_count_] = MdLoadTexturePixelArt(map.tileset_atlases[i]);
        bool valid = (atlases_[atlas_count_].id != 0);
#ifdef MD_SDL_GPU
        valid = valid || (atlases_[atlas_count_].sdl_tex != nullptr);
#endif
        if (!valid)
            fprintf(stderr, "[TileMap2D] atlas[%d] failed: %s\n", i, map.tileset_atlases[i]);
        ++atlas_count_;
    }
}

// ── Render ────────────────────────────────────────────────────────────────────

void TileMap2DRenderer::SetNpcSpriteSheet(const char* path) {
    if (!path || !init_) return;
    npc_atlas_slot_ = atlas_count_;  // first free binding slot
    if (npc_atlas_slot_ >= MAX_ATLAS) { npc_atlas_slot_ = -1; return; }
    npc_sprite_tex_ = MdLoadTexturePixelArt(path);  // uses stbi flip=1
    bool ok = (npc_sprite_tex_.sdl_tex != nullptr) || (npc_sprite_tex_.id != 0);
    if (!ok) { npc_atlas_slot_ = -1; return; }
    fprintf(stderr, "[TileMap2DRenderer] NPC sprite sheet: %s (slot %d)\n",
            path, npc_atlas_slot_);
}

// ── Goblin animation tables (fantasycore/animations/enemies/goblin.txt) ───────
// Format: {src_x, src_y, src_w, src_h, offset_x, offset_y}
// Indexed [frame_idx][direction 0-7].
struct GobFrame { short x,y,w,h,ox,oy; };

// [run] — 8 frames × 8 directions (duration=533ms, looped)
static const GobFrame RUN[8][8] = {
    {{1033,166,128,87,63,77},{992,521,98,92,38,85},{338,1375,75,104,30,89},{159,881,96,96,30,85},{306,159,120,86,54,77},{255,884,105,96,49,70},{699,891,72,97,27,71},{887,0,105,81,63,70}},
    {{961,613,128,93,77,85},{413,1376,102,103,40,93},{730,1596,84,108,35,96},{895,1707,106,111,25,93},{176,694,132,93,42,85},{1192,993,114,97,54,78},{1208,1090,86,99,36,76},{426,161,100,86,69,78}},
    {{1201,1601,96,109,59,104},{637,2416,97,136,42,124},{874,2707,103,144,39,132},{1018,2437,107,137,36,122},{438,2036,124,121,32,101},{0,1070,116,97,47,88},{453,336,115,89,58,87},{938,706,86,94,45,88}},
    {{806,2699,68,144,30,139},{80,2954,90,153,39,143},{117,2807,107,147,40,139},{1125,2442,103,137,34,130},{420,2543,89,138,26,121},{328,2277,96,129,33,117},{653,1926,112,116,51,121},{89,2268,86,130,42,130}},
    {{798,2843,73,156,25,150},{377,2824,103,148,34,140},{86,2400,109,131,38,128},{545,2160,93,126,31,121},{954,2299,84,133,27,120},{628,2552,94,139,32,127},{195,2667,107,140,44,140},{1213,2723,94,146,43,150}},
    {{170,2960,79,153,23,146},{707,2698,99,143,31,137},{424,2280,105,129,36,127},{739,2167,84,126,27,121},{1228,2446,74,137,21,122},{977,2713,88,145,30,131},{302,2675,111,142,48,142},{871,2851,99,150,47,148}},
    {{1065,2718,70,145,22,136},{1231,2312,84,134,29,129},{865,2049,96,122,35,122},{265,2148,80,124,26,119},{734,2424,68,136,18,121},{413,2681,80,143,27,128},{493,2684,107,142,49,135},{480,2826,96,148,47,138}},
    {{814,1596,98,108,66,98},{961,2051,89,122,41,116},{345,2153,91,124,35,114},{1001,1709,110,111,18,93},{994,1390,119,104,29,90},{507,608,103,93,45,81},{404,247,88,88,38,79},{1024,707,102,94,71,83}},
};

// [stance] — 4 frames × 8 directions (duration=800ms, back_forth → ping-pong)
static const GobFrame STANCE[4][8] = {
    {{142,977,111,96,68,84},{1106,801,81,95,34,85},{171,1582,86,106,33,87},{670,1487,96,105,22,84},{1070,993,122,97,41,84},{326,604,99,93,46,77},{418,698,82,94,33,76},{1017,341,96,90,63,77}},
    {{354,982,111,96,68,83},{625,701,82,94,34,84},{366,1586,87,106,34,87},{715,1383,96,104,22,83},{698,988,125,96,41,83},{810,519,99,92,47,76},{425,605,82,93,33,75},{705,250,96,89,63,76}},
    {{1187,803,110,95,69,82},{610,517,83,92,35,82},{811,1387,87,104,34,85},{898,1387,96,104,22,82},{465,889,121,95,40,82},{832,428,97,91,48,75},{909,521,83,92,34,72},{595,163,96,87,63,75}},
    {{707,703,110,94,70,81},{257,420,84,91,36,82},{251,1375,87,103,34,84},{766,1491,97,105,22,82},{817,705,121,94,39,81},{1122,431,96,91,47,74},{932,340,85,90,34,70},{211,159,95,86,64,74}},
};

// Map rot_y (atan2(dx,dz)) → Flare direction index 0-7.
// Flare dirs: 0=N(up-right), 1=NE, 2=E(right), 3=SE, 4=S(down-left), 5=SW, 6=W, 7=NW.
static int GoblinDir(float rot_y) {
    // Shift so that rot_y=0 (+Z, moving "up" in tile space) maps to dir=0 (N).
    float a = rot_y + 3.14159265f * 0.125f;  // offset by half-sector
    if (a < 0.f) a += 3.14159265f * 2.f;
    a = a - 3.14159265f * 2.f * floorf(a / (3.14159265f * 2.f));
    return (int)(a / (3.14159265f * 2.f / 8.f)) & 7;
}

void TileMap2DRenderer::SetNpcSprites(const float* tile_x, const float* tile_z,
                                       const float* rot_y,  const uint8_t* is_moving,
                                       int count, float now_s)
{
    npc_dot_count_ = count < MAX_NPC_DOTS ? count : MAX_NPC_DOTS;
    for (int i = 0; i < npc_dot_count_; ++i) {
        npc_dot_x_[i] = tile_x[i];
        npc_dot_z_[i] = tile_z[i];
        npc_rot_y_[i] = rot_y[i];
        npc_moving_[i] = is_moving[i];
    }
    npc_now_s_ = now_s;
}

void TileMap2DRenderer::Render(const FlareMap& map, float now_s,
                                float origin_x, float origin_y, float scale,
                                int vp_w, int vp_h, uint8_t layer_mask)
{
    if (!init_ || atlas_count_ == 0) return;
    if (map.layer_count == 0 || map.width <= 0 || map.height <= 0) return;

#ifdef MD_SDL_GPU
    if (sdl_init_) {
        md::GpuCommandBufferHandle cmd = md::GpuDevice::Get().AcquireCommandBuffer();
        if (!cmd) return;
        uint32_t sw = 0, sh = 0;
        md::GpuTextureHandle swap = md::GpuDevice::Get().AcquireSwapchainTexture(cmd, &sw, &sh);
        if (!swap) { md::GpuDevice::Get().Submit(cmd); return; }
        RenderSDLGPU(map, now_s, origin_x, origin_y, scale, vp_w, vp_h, layer_mask, cmd, swap);
#ifdef MD_SDL_GPU
        for (int oi = 0; oi < MAX_OVERLAY_BLITS; ++oi) {
            if (!overlay_blits_[oi].sdl_tex) continue;
            SDL_GPUBlitInfo bi {};
            bi.source.texture  = (md::GpuTextureHandle)overlay_blits_[oi].sdl_tex;
            bi.source.w        = (uint32_t)overlay_blits_[oi].w;
            bi.source.h        = (uint32_t)overlay_blits_[oi].h;
            bi.destination.texture = swap;
            bi.destination.x   = (uint32_t)overlay_blits_[oi].x;
            bi.destination.y   = (uint32_t)overlay_blits_[oi].y;
            bi.destination.w   = (uint32_t)overlay_blits_[oi].w;
            bi.destination.h   = (uint32_t)overlay_blits_[oi].h;
            bi.load_op         = SDL_GPU_LOADOP_LOAD;
            bi.filter          = SDL_GPU_FILTER_NEAREST;
            GpuBlitTexture(cmd, bi);
        }
#endif
        md::GpuDevice::Get().Submit(cmd);
        return;
    }
#endif

    if (!prog_) return;

    // ── Collect + sort tiles ──────────────────────────────────────────────────
    static Tile2D tiles[MAX_TILES];
    int n = 0;
    for (int li = 0; li < map.layer_count && n < MAX_TILES; ++li) {
        if (!(layer_mask & (1u << li))) continue;
        const TileMapLayer& layer = map.layers[li];
        using LT = md::flare::LayerType;
        if (layer.type == LT::COLLISION) continue;
        if (layer.type != LT::BACKGROUND && layer.type != LT::FRINGE && layer.type != LT::OBJECT) continue;
        int prio = LayerPrio2D(layer.type);
        for (int row = 0; row < map.height && n < MAX_TILES; ++row) {
            for (int col = 0; col < map.width && n < MAX_TILES; ++col) {
                int tid = layer.tiles[row * MAX_MAP_WIDTH + col];
                if (tid == 0 || !map.meta.Find(tid)) continue;
                tiles[n++] = { col, row, tid, prio, Tile2DPrio(col, row, prio) };
            }
        }
    }
    qsort(tiles, (size_t)n, sizeof(Tile2D), Tile2DCmp);

    // ── Build instance buffer (stride=36) ─────────────────────────────────────
    static uint8_t ibuf[MAX_TILES * STRIDE];
    int ni = 0;

    for (int i = 0; i < n; ++i) {
        const Tile2D& t    = tiles[i];
        const TileMeta* tm = map.meta.Find(t.tid);
        if (!tm) continue;

        int16_t sx = tm->src_x, sy = tm->src_y;
        if (now_s > 0.0f) {
            const TileAnim* anim = map.meta.FindAnim(t.tid);
            if (anim && anim->total_ms > 0) {
                uint32_t ms    = (uint32_t)(now_s * 1000.0f) % anim->total_ms;
                uint32_t accum = 0;
                for (int fi = 0; fi < anim->frame_count; ++fi) {
                    accum += anim->frames[fi].duration_ms;
                    if (ms < accum) { sx = anim->frames[fi].src_x; sy = anim->frames[fi].src_y; break; }
                }
            }
        }

        uint8_t aidx = tm->atlas_idx < (uint8_t)atlas_count_ ? tm->atlas_idx : 0;
        const MdTexture& atl = atlases_[aidx];
        if (!atl.id || atl.w <= 0 || atl.h <= 0) continue;
        if (sx + tm->w > atl.w || sy + tm->h > atl.h) continue;

        float ax   = (float)((t.col - t.row) * TILE_W_HALF) * scale + origin_x;
        float ay   = (float)((t.col + t.row) * TILE_H_HALF) * scale + origin_y;
        float x_tl = roundf(ax - (float)tm->offset_x * scale);
        float y_tl = roundf(ay - (float)tm->offset_y * scale);
        float sw2  = (float)tm->w * scale;
        float sh2  = (float)tm->h * scale;
        float u0 = (float)sx / (float)atl.w;
        float u1 = (float)(sx + tm->w) / (float)atl.w;
        float v0 = 1.0f - (float)sy / (float)atl.h;
        float v1 = 1.0f - (float)(sy + tm->h) / (float)atl.h;
        float ai = (float)aidx;

        uint8_t* p = ibuf + ni * STRIDE;
        memcpy(p +  0, &x_tl, 4);  memcpy(p +  4, &y_tl, 4);
        memcpy(p +  8, &sw2,  4);  memcpy(p + 12, &sh2,  4);
        memcpy(p + 16, &u0,   4);  memcpy(p + 20, &v0,   4);
        memcpy(p + 24, &u1,   4);  memcpy(p + 28, &v1,   4);
        memcpy(p + 32, &ai,   4);
        ++ni;
    }
    n = ni;

    // ── Upload + draw ─────────────────────────────────────────────────────────
    glBindBuffer(GL_ARRAY_BUFFER, inst_vbo_);
    glBufferSubData(GL_ARRAY_BUFFER, 0, (GLsizeiptr)(n * STRIDE), ibuf);

    GLint prev_prog = 0;
    glGetIntegerv(GL_CURRENT_PROGRAM, &prev_prog);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glUseProgram(prog_);

    float vp[2] = { (float)vp_w, (float)vp_h };
    glUniform2fv(loc_viewport_, 1, vp);

    for (int i = 0; i < atlas_count_ && i < MAX_ATLAS; ++i) {
        glActiveTexture(GL_TEXTURE0 + i);
        glBindTexture(GL_TEXTURE_2D, atlases_[i].id);
        if (loc_atlas_[i] >= 0) glUniform1i(loc_atlas_[i], i);
    }

    glBindVertexArray(vao_);
    glDrawArraysInstanced(GL_TRIANGLE_FAN, 0, 4, n);
    glBindVertexArray(0);

    // Restore GL state for any subsequent draw calls.
    glUseProgram((GLuint)prev_prog);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, 0);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glEnable(GL_DEPTH_TEST);
}

// ── Atlas accessors (non-inline — struct layout differs between engine and editor) ──

MdTexture TileMap2DRenderer::GetAtlas(int idx) const {
    if (idx < 0 || idx >= atlas_count_) return {};
    return atlases_[idx];
}
int TileMap2DRenderer::GetAtlasCount() const { return atlas_count_; }

// ── RenderSDLGPU ──────────────────────────────────────────────────────────────

#ifdef MD_SDL_GPU
void TileMap2DRenderer::RenderSDLGPU(const FlareMap& map, float now_s,
                                      float origin_x, float origin_y, float scale,
                                      int vp_w, int vp_h, uint8_t layer_mask,
                                      md::GpuCommandBufferHandle cmd, md::GpuTextureHandle swap_tex)
{
    // Collect + sort tiles (SDL_GPU path owns its own scratch to avoid aliasing
    // with the static tiles[] in the GL path when both are compiled).
    // FL-2+FL-3: collect tiles into up to 3 groups:
    //   tiles[]      — main batch (back layers + fringe/object when no split)
    //   over_tiles[] — FL-3 overhead layers (rendered last, above entities)
    //   fade_tiles[] — FL-2 OBJECT tiles at player position (transparent, top)
    static Tile2D tiles     [MAX_TILES];
    static Tile2D over_tiles[MAX_TILES];
    static Tile2D fade_tiles[8];
    int n = 0, n_over = 0, n_fade = 0;
    for (int li = 0; li < map.layer_count && n < MAX_TILES; ++li) {
        if (!(layer_mask & (1u << li))) continue;
        const TileMapLayer& layer = map.layers[li];
        using LT = md::flare::LayerType;
        if (layer.type == LT::COLLISION) continue;
        if (layer.type != LT::BACKGROUND && layer.type != LT::FRINGE && layer.type != LT::OBJECT) continue;
        int prio = LayerPrio2D(layer.type);
        // FL-3: layers above object_layer_idx → overhead batch (phase 4)
        const bool is_overhead = (object_layer_idx_ >= 0 && li > object_layer_idx_);
        for (int row = 0; row < map.height && n < MAX_TILES; ++row) {
            for (int col = 0; col < map.width && n < MAX_TILES; ++col) {
                int tid = layer.tiles[row * MAX_MAP_WIDTH + col];
                if (tid == 0 || !map.meta.Find(tid)) continue;
                // FL-2: OBJECT tiles at player position → separate fade batch
                if (layer.type == LT::OBJECT &&
                    col == player_tile_col_ && row == player_tile_row_ &&
                    n_fade < 8) {
                    fade_tiles[n_fade++] = { col, row, tid, prio, Tile2DPrio(col, row, prio) };
                } else if (is_overhead && n_over < MAX_TILES) {
                    // FL-3: overhead tiles go to separate batch (drawn after NPC sprites)
                    over_tiles[n_over++] = { col, row, tid, prio, Tile2DPrio(col, row, prio) };
                } else {
                    tiles[n++] = { col, row, tid, prio, Tile2DPrio(col, row, prio) };
                }
            }
        }
    }
    qsort(tiles,      (size_t)n,      sizeof(Tile2D), Tile2DCmp);
    qsort(over_tiles, (size_t)n_over, sizeof(Tile2D), Tile2DCmp);
    qsort(fade_tiles, (size_t)n_fade, sizeof(Tile2D), Tile2DCmp);

    // Corners for 2 CCW triangles (TRIANGLE_LIST).
    static const float CORNERS[6][2] = {
        {0.f,0.f}, {1.f,0.f}, {1.f,1.f},
        {0.f,0.f}, {1.f,1.f}, {0.f,1.f}
    };

    // Build flat vertex buffer: 6 verts × STRIDE_SDL bytes per tile.
    static uint8_t scratch[MAX_TILES * 6 * STRIDE_SDL];
    int ni = 0;

    for (int i = 0; i < n && ni < MAX_TILES; ++i) {
        const Tile2D& t    = tiles[i];
        const TileMeta* tm = map.meta.Find(t.tid);
        if (!tm) continue;

        int16_t sx = tm->src_x, sy = tm->src_y;
        if (now_s > 0.0f) {
            const TileAnim* anim = map.meta.FindAnim(t.tid);
            if (anim && anim->total_ms > 0) {
                uint32_t ms    = (uint32_t)(now_s * 1000.0f) % anim->total_ms;
                uint32_t accum = 0;
                for (int fi = 0; fi < anim->frame_count; ++fi) {
                    accum += anim->frames[fi].duration_ms;
                    if (ms < accum) { sx = anim->frames[fi].src_x; sy = anim->frames[fi].src_y; break; }
                }
            }
        }

        uint8_t aidx = tm->atlas_idx < (uint8_t)atlas_count_ ? tm->atlas_idx : 0;
        const MdTexture& atl = atlases_[aidx];
        if (!atl.sdl_tex || atl.w <= 0 || atl.h <= 0) continue;
        if (sx + tm->w > atl.w || sy + tm->h > atl.h) continue;

        float ax   = (float)((t.col - t.row) * TILE_W_HALF) * scale + origin_x;
        float ay   = (float)((t.col + t.row) * TILE_H_HALF) * scale + origin_y;
        float x_tl = roundf(ax - (float)tm->offset_x * scale);
        float y_tl = roundf(ay - (float)tm->offset_y * scale);
        float sw   = (float)tm->w * scale;
        float sh   = (float)tm->h * scale;

        // UV: stbi flip active → v_gl = 1 − y_file / H.
        float u0 = (float)sx / (float)atl.w;
        float u1 = (float)(sx + tm->w) / (float)atl.w;
        float v0 = 1.0f - (float)sy / (float)atl.h;
        float v1 = 1.0f - (float)(sy + tm->h) / (float)atl.h;
        float ai = (float)aidx;

        for (int vi = 0; vi < 6; ++vi) {
            float* v = (float*)(scratch + ((size_t)ni * 6 + (size_t)vi) * (size_t)STRIDE_SDL);
            v[0]  = CORNERS[vi][0];  // a_corner.x
            v[1]  = CORNERS[vi][1];  // a_corner.y
            v[2]  = x_tl;           // a_screen_tl.x
            v[3]  = y_tl;           // a_screen_tl.y
            v[4]  = sw;             // a_screen_size.x
            v[5]  = sh;             // a_screen_size.y
            v[6]  = u0;             // a_uv_rect.x
            v[7]  = v0;             // a_uv_rect.y
            v[8]  = u1;             // a_uv_rect.z
            v[9]  = v1;             // a_uv_rect.w
            v[10] = ai;             // a_atlas_idx
        }
        ++ni;
    }

    // ── NPC overlay: animated goblin sprite or fallback white dot ────────────
    if (npc_dot_count_ > 0 && ni < MAX_TILES) {
        bool use_sprite = (npc_atlas_slot_ >= 0) && npc_sprite_tex_.sdl_tex;
        float ai_sprite = (float)npc_atlas_slot_;
        float ai_dot    = (float)atlas_count_;  // dummy white

        // Pre-compute animation frame indices from now_s.
        // Run: 8 frames over 533ms. Stance: 4 frames ping-pong over 800ms.
        float ms = npc_now_s_ * 1000.f;
        int run_fi   = (int)(fmodf(ms, 533.f) / (533.f / 8.f)) & 7;
        int stance_f = (int)(fmodf(ms, 800.f) / (800.f / 4.f));
        if (stance_f > 3) stance_f = 3;

        for (int di = 0; di < npc_dot_count_ && ni < MAX_TILES; ++di) {
            float col = npc_dot_x_[di], row = npc_dot_z_[di];
            float ax  = (float)((col - row) * (float)TILE_W_HALF) * scale + origin_x;
            float ay  = (float)((col + row) * (float)TILE_H_HALF) * scale + origin_y;

            float x_tl, y_tl, sw, sh, u0, v0, u1, v1, ai;

            if (use_sprite) {
                int dir = GoblinDir(npc_rot_y_[di]);
                const GobFrame& f = npc_moving_[di]
                                    ? RUN[run_fi][dir]
                                    : STANCE[stance_f][dir];
                sw   = (float)f.w * scale;
                sh   = (float)f.h * scale;
                x_tl = roundf(ax - (float)f.ox * scale);
                y_tl = roundf(ay - (float)f.oy * scale);
                u0   = (float)f.x / (float)NPC_SHEET_W;
                u1   = (float)(f.x + f.w) / (float)NPC_SHEET_W;
                v0   = 1.f - (float)f.y / (float)NPC_SHEET_H;
                v1   = 1.f - (float)(f.y + f.h) / (float)NPC_SHEET_H;
                ai   = ai_sprite;
            } else {
                float dpx = 10.f;
                sw = sh = dpx;
                x_tl = ax + (float)TILE_W_HALF * scale * 0.5f - dpx * 0.5f;
                y_tl = ay + (float)TILE_H_HALF * scale * 0.5f - dpx * 0.5f;
                u0 = 0.f; v0 = 0.f; u1 = 1.f; v1 = 1.f;
                ai = ai_dot;
            }

            for (int vi = 0; vi < 6; ++vi) {
                float* v = (float*)(scratch + ((size_t)ni * 6 + (size_t)vi) * (size_t)STRIDE_SDL);
                v[0]=CORNERS[vi][0]; v[1]=CORNERS[vi][1];
                v[2]=x_tl; v[3]=y_tl; v[4]=sw; v[5]=sh;
                v[6]=u0; v[7]=v0; v[8]=u1; v[9]=v1; v[10]=ai;
            }
            ++ni;
        }
    }

    // FL-3: append overhead tiles AFTER main tiles + NPC dots (phase 4 — above entities).
    for (int fi = 0; fi < n_over && ni < MAX_TILES; ++fi) {
        const Tile2D& t    = over_tiles[fi];
        const TileMeta* tm = map.meta.Find(t.tid);
        if (!tm) continue;
        const MdTexture& atl = atlases_[tm->atlas_idx < (uint8_t)atlas_count_ ? tm->atlas_idx : 0];
        if (!atl.sdl_tex || atl.w <= 0 || atl.h <= 0) continue;
        float ax  = (float)((t.col - t.row) * TILE_W_HALF) * scale + origin_x;
        float ay  = (float)((t.col + t.row) * TILE_H_HALF) * scale + origin_y;
        float x_tl = roundf(ax - (float)tm->offset_x * scale);
        float y_tl = roundf(ay - (float)tm->offset_y * scale);
        float sw   = (float)tm->w * scale, sh = (float)tm->h * scale;
        float u0 = (float)tm->src_x / (float)atl.w;
        float u1 = (float)(tm->src_x + tm->w) / (float)atl.w;
        float v0 = 1.f - (float)tm->src_y / (float)atl.h;
        float v1 = 1.f - (float)(tm->src_y + tm->h) / (float)atl.h;
        float ai = (float)(tm->atlas_idx < (uint8_t)atlas_count_ ? tm->atlas_idx : 0);
        for (int vi = 0; vi < 6; ++vi) {
            float* v = (float*)(scratch + ((size_t)ni * 6 + (size_t)vi) * (size_t)STRIDE_SDL);
            v[0]=CORNERS[vi][0]; v[1]=CORNERS[vi][1];
            v[2]=x_tl; v[3]=y_tl; v[4]=sw; v[5]=sh;
            v[6]=u0; v[7]=v0; v[8]=u1; v[9]=v1; v[10]=ai;
        }
        ++ni;
    }
    const int ni_overhead_end = ni;  // vertex count after overhead tiles

    // FL-2: append fade tiles to scratch AFTER main tiles + NPC dots + overhead.
    // ni_main_end = vertex count before fade tiles (used for two-pass draw split).
    const int ni_main_end = ni;
    for (int fi = 0; fi < n_fade && ni < MAX_TILES; ++fi) {
        const Tile2D& t    = fade_tiles[fi];
        const TileMeta* tm = map.meta.Find(t.tid);
        if (!tm) continue;
        const MdTexture& atl = atlases_[tm->atlas_idx < (uint8_t)atlas_count_ ? tm->atlas_idx : 0];
        if (!atl.sdl_tex || atl.w <= 0 || atl.h <= 0) continue;
        float ax  = (float)((t.col - t.row) * TILE_W_HALF) * scale + origin_x;
        float ay  = (float)((t.col + t.row) * TILE_H_HALF) * scale + origin_y;
        float x_tl = roundf(ax - (float)tm->offset_x * scale);
        float y_tl = roundf(ay - (float)tm->offset_y * scale);
        float sw   = (float)tm->w * scale;
        float sh   = (float)tm->h * scale;
        float u0 = (float)tm->src_x / (float)atl.w;
        float u1 = (float)(tm->src_x + tm->w) / (float)atl.w;
        float v0 = 1.f - (float)tm->src_y / (float)atl.h;
        float v1 = 1.f - (float)(tm->src_y + tm->h) / (float)atl.h;
        float ai = (float)(tm->atlas_idx < (uint8_t)atlas_count_ ? tm->atlas_idx : 0);
        for (int vi = 0; vi < 6; ++vi) {
            float* v = (float*)(scratch + ((size_t)ni * 6 + (size_t)vi) * (size_t)STRIDE_SDL);
            v[0]=CORNERS[vi][0]; v[1]=CORNERS[vi][1];
            v[2]=x_tl; v[3]=y_tl; v[4]=sw; v[5]=sh;
            v[6]=u0; v[7]=v0; v[8]=u1; v[9]=v1; v[10]=ai;
        }
        ++ni;
    }

    // ── Correct order: copy pass first, then render pass ─────────────────────
    // SDL_GPU requires the copy pass to precede the render pass on the same cmd.
    if (ni > 0) {
        void* ptr = sdl_vbuf_.MapWrite();
        if (ptr) {
            memcpy(ptr, scratch, (size_t)ni * 6u * (size_t)STRIDE_SDL);
            sdl_vbuf_.Unmap();
        }
        sdl_vbuf_.Upload(cmd);  // opens + closes a copy pass on cmd
    }

    // Render pass: clear swapchain + optionally draw tiles.
    GpuCommandBuffer cb;
    GpuCommandBuffer::ColorPassDesc cpd;
    cpd.cmd            = cmd;
    cpd.color_tex[0]      = swap_tex;
    cpd.clear_color[0] = 0.08f;
    cpd.clear_color[1] = 0.08f;
    cpd.clear_color[2] = 0.12f;
    cpd.clear_color[3] = 1.0f;
    cpd.load_color     = false;
    cb.BeginColorPass(cpd);

    if (ni > 0) {
        cb.BindPipeline(&sdl_pipeline_);
        cb.BindVertexBuffer(&sdl_vbuf_);

        // Bind 4 atlas samplers; pad unused slots with the 1×1 dummy texture.
        // If NPC sprite sheet is loaded, override its slot with the sprite texture.
        SDL_GPUTextureSamplerBinding bindings[4];
        for (int i = 0; i < 4; ++i) {
            if (i == npc_atlas_slot_ && npc_sprite_tex_.sdl_tex) {
                bindings[i].texture = (md::GpuTextureHandle)npc_sprite_tex_.sdl_tex;
                bindings[i].sampler = (SDL_GPUSampler*)npc_sprite_tex_.sdl_sampler;
            } else {
                bool has = (i < atlas_count_) && atlases_[i].sdl_tex;
                bindings[i].texture = has ? (md::GpuTextureHandle)atlases_[i].sdl_tex
                                          : (md::GpuTextureHandle)sdl_dummy_tex_;
                bindings[i].sampler = has ? (SDL_GPUSampler*)atlases_[i].sdl_sampler
                                          : (SDL_GPUSampler*)sdl_dummy_sampler_;
            }
        }
        cb.BindFragmentSamplers(0, bindings, 4);

        // FL-2: alpha_mod passed via vertex UBO (set=1, binding=0), 3rd float.
        // UBO: {vec2 viewport, float alpha_mod, float pad} = 16 bytes std140.
        // Pass 1: main tiles + NPC + overhead — alpha_mod=1.0
        float vp_ubo[4] = { (float)vp_w, (float)vp_h, 1.0f, 0.f };
        cb.PushVertexUniforms(0, vp_ubo, 16);
        if (ni_overhead_end > 0) cb.Draw((uint32_t)(ni_overhead_end * 6));

        // Pass 2: fade tiles at player position — semi-transparent (alpha_mod=fade_alpha_).
        const int ni_fade_count = ni - ni_overhead_end;
        if (ni_fade_count > 0) {
            float vp_fade[4] = { (float)vp_w, (float)vp_h, fade_alpha_, 0.f };
            cb.PushVertexUniforms(0, vp_fade, 16);
            cb.Draw((uint32_t)(ni_fade_count * 6), (uint32_t)(ni_overhead_end * 6));
        }
    }
    cb.EndPass();
}
#endif // MD_SDL_GPU

// ── Overlay blits ─────────────────────────────────────────────────────────────

void TileMap2DRenderer::SetOverlayBlit(int slot, void* sdl_tex,
                                        int x, int y, int w, int h) {
    if (slot < 0 || slot >= MAX_OVERLAY_BLITS) return;
    overlay_blits_[slot] = { sdl_tex, x, y, w, h };
}

void TileMap2DRenderer::ClearOverlayBlit(int slot) {
    if (slot < 0 || slot >= MAX_OVERLAY_BLITS) return;
    overlay_blits_[slot] = {};
}

#ifdef MD_SDL_GPU
void TileMap2DRenderer::RenderToTarget(const FlareMap& map, float now_s,
                                        float origin_x, float origin_y, float scale,
                                        int vp_w, int vp_h, uint8_t layer_mask,
                                        md::GpuCommandBufferHandle cmd, md::GpuTextureHandle target_tex)
{
    if (!init_ || !sdl_init_ || !cmd || !target_tex) return;
    if (atlas_count_ == 0) return;
    if (map.layer_count == 0 || map.width <= 0 || map.height <= 0) return;
    RenderSDLGPU(map, now_s, origin_x, origin_y, scale, vp_w, vp_h, layer_mask, cmd, target_tex);
}
#endif

} // namespace md::flare


#pragma once
#include <monkey_dust/platform/md_fs.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>

// RenderQualityConfig — per-type cull radii + quality toggles.
// Inspired by Lineage 2 L2Configuration: separate TerrainCR / StaticMeshCR /
// ActorCR / StaticMeshLodCR + GL2RenderDeco + GL2TextureDetail per hardware preset.
//
// Read at render time via RenderQualityConfig::Get().

struct RenderQualityConfig {
    // ── Cull radii (metres) ─────────────────────────────────────────────────
    float terrain_cr_m    = 3000.f; // terrain chunk draw distance
    // mesh_lod0/1/2_cr_m: DOUBLE DUTY (task #182g audit, 2026-07-19 — not
    // previously documented) — originally "static props LOD" per the names
    // below, but game/src/render/npc_render.cpp's terrain draw loop ALSO
    // reuses these exact 3 fields as the GAME's terrain chunk mesh-LOD
    // distance thresholds (SceneRender's fixed 9x9 window). The EDITOR's
    // World3D 64x64 aerial viewport does NOT use these — it has its own
    // separate, deliberately larger hardcoded d0sq/d1sq/d2sq (1200/3500/
    // 8000) in tools/editor/editor_world_3d_sdlgpu.cpp. See also
    // engine/include/monkey_dust/world/terrain_chunk.h's removed
    // TERRAIN_LOD_DIST comment for the full 2-systems-not-3 picture.
    float mesh_lod0_cr_m  =  500.f; // static props LOD0 (POM close) + game terrain LOD0
    float mesh_lod1_cr_m  = 1500.f; // static props LOD1 + game terrain LOD1
    float mesh_lod2_cr_m  = 3000.f; // static props LOD2 + game terrain LOD2
    float actor_cr_m      =  150.f; // NPC/character draw distance (cull.comp far_sq)
    float actor_anim_t2_m =  150.f; // NPC animation T2 (LOD2 skip skinning)

    // ── Prop-type distances (metres) — replaces npc_render hardcoded consts ──
    float prop_rock_m     =  300.f;
    float prop_formation_m=  450.f;
    float prop_hat_rock_m =  600.f;
    float prop_veg_m      =  200.f;  // yucca, canyon_rock
    float prop_tree_m     =  350.f;

    // ── Quality toggles (L2: GL2RenderDeco, GL2TextureDetail) ──────────────
    bool    render_deco   = false;  // false = skip all decorative props (vegetation)
    uint8_t tex_detail    = 1;      // 0=full mips, 1=+1 mip bias, 2=+2 mip bias
    uint8_t _pad[2]       = {};

    // ── Singleton ───────────────────────────────────────────────────────────
    static RenderQualityConfig& Get() noexcept {
        static RenderQualityConfig cfg;
        return cfg;
    }

    // Optional runtime tuning without a C++ recompile (user request,
    // 2026-07-18 terrain-seam investigation — every numeric experiment that
    // session needed a full ninja rebuild). Call once at startup. Plain
    // key=value lines (one per line, '#' starts a comment, blank lines
    // ignored) — no external JSON library per project convention. Missing
    // file is silent/expected (most runs have no overrides).
    static void LoadOverrides(const char* path = "game/data/render_quality_overrides.txt") noexcept {
        uint32_t size = 0;
        char* buf = md::fs_read_alloc(path, &size);
        if (!buf) return;
        RenderQualityConfig& c = Get();
        char* line = buf;
        while (line < buf + size) {
            char* nl = (char*)memchr(line, '\n', (buf + size) - line);
            char* end = nl ? nl : (buf + size);
            *end = '\0';
            char* eq = strchr(line, '=');
            if (eq && line[0] != '#') {
                *eq = '\0';
                const char* key = line;
                const char* val = eq + 1;
                char* vend = nullptr;
                const float fval = strtof(val, &vend);
                if (vend == val) { fprintf(stderr, "[RenderQualityConfig] not a number, line skipped: %s=%s\n", key, val); line = end + 1; continue; }
                if      (!strcmp(key, "terrain_cr_m"))    c.terrain_cr_m    = fval;
                else if (!strcmp(key, "mesh_lod0_cr_m"))  c.mesh_lod0_cr_m  = fval;
                else if (!strcmp(key, "mesh_lod1_cr_m"))  c.mesh_lod1_cr_m  = fval;
                else if (!strcmp(key, "mesh_lod2_cr_m"))  c.mesh_lod2_cr_m  = fval;
                else if (!strcmp(key, "actor_cr_m"))      c.actor_cr_m      = fval;
                else if (!strcmp(key, "actor_anim_t2_m")) c.actor_anim_t2_m = fval;
                else if (!strcmp(key, "prop_rock_m"))     c.prop_rock_m     = fval;
                else if (!strcmp(key, "prop_formation_m"))c.prop_formation_m= fval;
                else if (!strcmp(key, "prop_hat_rock_m")) c.prop_hat_rock_m = fval;
                else if (!strcmp(key, "prop_veg_m"))      c.prop_veg_m      = fval;
                else if (!strcmp(key, "prop_tree_m"))     c.prop_tree_m     = fval;
                else if (!strcmp(key, "render_deco"))     c.render_deco     = fval != 0.f;
                else if (!strcmp(key, "tex_detail"))       c.tex_detail      = (uint8_t)fval;
                else    fprintf(stderr, "[RenderQualityConfig] unknown override key: %s\n", key);
            }
            line = end + 1;
        }
        md::fs_free(buf);
        fprintf(stdout, "[RenderQualityConfig] loaded overrides from %s\n", path);
    }
};

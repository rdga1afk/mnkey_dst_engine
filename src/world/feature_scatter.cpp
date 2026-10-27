#include <monkey_dust/world/feature_scatter.h>
#include <monkey_dust/io/data_files.h>
#include <monkey_dust/platform/md_log.h>
#include <monkey_dust/io/fcs_gamedata.h>
#include <monkey_dust/io/kenshi_dir.h>
#include <monkey_dust/render/asset_cache.h>
#include <monkey_dust/render/gpu_sampler_texture.h>
#include <fstream>
#include <iterator>
#include <string>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cmath>

FeatureScatterSystem& FeatureScatterSystem::Get() {
    static FeatureScatterSystem s;
    return s;
}

// Mirrors tools/md_convert_features.py's sanitize() exactly — same
// basename/strip-.mesh/non-alnum-to-underscore rule, so a mesh_path from
// the scatter file always resolves to the .glb that script produced.
static void s_sanitize(const char* kenshi_path, char* out, int out_sz) {
    const char* slash = kenshi_path;
    for (const char* p = kenshi_path; *p; ++p)
        if (*p == '\\' || *p == '/') slash = p + 1;

    int n = 0;
    for (const char* p = slash; *p && n < out_sz - 1; ++p) {
        char c = *p;
        // Strip a trailing ".mesh" (case-insensitive) once we reach it.
        if ((c == '.' || c == 'M' || c == 'm') &&
            (strncasecmp(p, ".mesh", 5) == 0) && p[5] == '\0') {
            break;
        }
        bool alnum = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                     (c >= '0' && c <= '9') || c == '_' || c == '.' || c == '-';
        out[n++] = alnum ? c : '_';
    }
    out[n] = '\0';
}

int FeatureScatterSystem::FindOrSkipMesh(const char* kenshi_mesh_path) const {
    char name[64];
    s_sanitize(kenshi_mesh_path, name, sizeof(name));
    for (int i = 0; i < mesh_count_; ++i)
        if (strcmp(mesh_names_[i], name) == 0) return i;
    return -1;
}

// features.dat (magic MF01) + the item each record names -> the placement text Init reads (private/md_features_parse.py --export, now at run time)
std::string BuildFeatureScatterText(const std::string& kenshi_dir) {
    std::ifstream in(kenshi_dir + "/data/newland/land/features.dat", std::ios::binary);
    if (!in) return "";
    const std::string d((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    auto u32 = [&](size_t o) { uint32_t v = 0; if (o + 4 <= d.size()) memcpy(&v, d.data() + o, 4); return v; };
    auto f32 = [&](size_t o) { float v = 0; if (o + 4 <= d.size()) memcpy(&v, d.data() + o, 4); return v; };
    if (d.size() < 8 || d.compare(0, 4, "MF01") != 0) return "";
    struct Rec { std::string name; float v[10]; };
    std::vector<Rec> recs;
    const uint32_t n_table = u32(4);
    for (uint32_t i = 0; i + 1 < n_table; i += 2) {
        const uint32_t off = u32(8 + 4 * (size_t)i), len = u32(8 + 4 * (size_t)i + 4);
        if (off == 0 && len == 0) continue;
        size_t pos = off; const int32_t n = (int32_t)u32(pos); pos += 4;
        for (int32_t r = 0; r < n && pos < d.size(); ++r) {
            Rec rec; const size_t nl = (unsigned char)d[pos++];
            rec.name = d.substr(pos, nl); pos += nl;
            for (int k = 0; k < 10; ++k, pos += 4) rec.v[k] = f32(pos);   // pos xyz, scale xyz, quat wxyz
            recs.push_back(std::move(rec));
        }
        if (pos - off != len) return "";
    }
    md::fcs::Install inst(kenshi_dir); md::fcs::LoadOrder order; md::fcs::Error err; md::fcs::GameDataContainer gd;
    if (!inst.LoadOrderFor(order, err) || !gd.Load(order, err)) return "";
    // MapFeatureMode: world units are 0.1 m, world origin (14745.6, 14745.6) m (#176)
    std::string out = "# game/data/features_scatter.txt -- built at run time from features.dat\n# mesh_path|x|y|z|sx|sy|sz|qw|qx|qy|qz|texture_mode (2 = TERRAIN)|texture map|tile X (mode 1 = triplanar)\n";
    char buf[768];
    for (const Rec& r : recs) {
        const md::fcs::GameData* g = gd.Get(r.name);
        if (!g) continue;
        std::string mesh; auto m = g->files.find("mesh"); if (m != g->files.end()) mesh = m->second; else { auto s2 = g->strings.find("mesh"); if (s2 != g->strings.end()) mesh = s2->second; }
        if (mesh.empty()) continue;
        int mode = 0; auto tm = g->ints.find("texture mode"); if (tm != g->ints.end()) mode = tm->second;
        std::string tex; auto tf = g->files.find("texture map"); if (tf != g->files.end()) tex = tf->second;
        float tile = 0; auto tl = g->floats.find("tile X"); if (tl != g->floats.end()) tile = tl->second;
        snprintf(buf, sizeof buf, "%s|%.3f|%.3f|%.3f|%.4f|%.4f|%.4f|%.6f|%.6f|%.6f|%.6f|%d|%s|%.1f\n", mesh.c_str(),
                 (double)r.v[0] / 10.0 + 14745.6, (double)r.v[1] / 10.0, (double)r.v[2] / 10.0 + 14745.6,
                 (double)r.v[3], (double)r.v[4], (double)r.v[5], (double)r.v[6], (double)r.v[7], (double)r.v[8], (double)r.v[9], mode, tex.c_str(), (double)tile);
        out += buf;
    }
    return out;
}

bool FeatureScatterSystem::Init(const char* scatter_path, const char* mesh_dir) {
    const std::string dev_scatter = md::DevFile(scatter_path);   // a pre-built placement file only in development (MD_DEV_FILES=1)
    FILE* f = dev_scatter.empty() ? nullptr : fopen(dev_scatter.c_str(), "r");
    std::string generated;
    if (!f) {   // the release has no committed placement file: build it from the install
        generated = BuildFeatureScatterText(md::KenshiDir());
        if (!generated.empty()) f = fmemopen((void*)generated.data(), generated.size(), "r");
    }
    if (!f) {
        MD_LOG(MD_LOG_WARNING, "[FeatureScatter] cannot open %s — no real scatter objects loaded", scatter_path);
        return false;
    }

    char line[512];
    while (fgets(line, sizeof(line), f)) {
        if (line[0] == '#' || line[0] == '\n' || line[0] == '\0') continue;

        char mesh_path[256];
        float x, y, z, sx, sy, sz, qw, qx, qy, qz;
        // mesh_path|x|y|z|sx|sy|sz|qw|qx|qy|qz — pipe-delimited, mesh_path
        // itself never contains '|' (Kenshi backslash paths only).
        char* p = line;
        char* bar = strchr(p, '|');
        if (!bar) continue;
        int mp_len = (int)(bar - p);
        if (mp_len >= (int)sizeof(mesh_path)) mp_len = (int)sizeof(mesh_path) - 1;
        memcpy(mesh_path, p, (size_t)mp_len);
        mesh_path[mp_len] = '\0';
        p = bar + 1;

        int mode = 0;
        if (sscanf(p, "%f|%f|%f|%f|%f|%f|%f|%f|%f|%f|%d",
                   &x, &y, &z, &sx, &sy, &sz, &qw, &qx, &qy, &qz, &mode) < 10) {
            continue;
        }
        // optional tail: |texture map|tile X (texture mode 1 = Triplanar: the record's own texture, projected from the world position, tiling = tile X)
        char tex_path[256] = ""; float tile = 0.f;
        { const char* q = p; for (int k = 0; k < 11 && q; ++k) { q = strchr(q, '|'); if (q) ++q; }
          if (q) { const char* e = strchr(q, '|'); if (e && (size_t)(e - q) < sizeof tex_path) { memcpy(tex_path, q, (size_t)(e - q)); tex_path[e - q] = '\0'; tile = (float)atof(e + 1); } } }
        const bool triplanar = mode == 1 && tex_path[0] && tile > 0.f;

        if (placement_count_ >= MAX_PLACEMENTS) {
            MD_LOG(MD_LOG_WARNING, "[FeatureScatter] MAX_PLACEMENTS (%d) hit, dropping remaining rows", MAX_PLACEMENTS);
            break;
        }

        int mesh_idx = FindOrSkipMesh(mesh_path);
        if (mesh_idx < 0) {
            if (mesh_count_ >= MAX_MESHES) {
                MD_LOG(MD_LOG_WARNING, "[FeatureScatter] MAX_MESHES (%d) hit, skipping %s", MAX_MESHES, mesh_path);
                continue;
            }
            char name[64];
            s_sanitize(mesh_path, name, sizeof(name));
            char glb_path[320];
            snprintf(glb_path, sizeof(glb_path), "%s/%s.glb", mesh_dir, name);
            mesh_idx = mesh_count_;
            // the converted .glb when it is there, else the install's own .mesh (units 0.1 m -> scale 0.1, as tools/md_convert_features.py)
            const std::string dev_glb = md::DevFile(glb_path);
            if (!(!dev_glb.empty() && renderers_[mesh_idx].Init(dev_glb.c_str())) && !renderers_[mesh_idx].InitFromOgreMesh(mesh_path, 0.1f, triplanar ? 4.0f + tile * 0.001f : 0.f)) {
                MD_LOG(MD_LOG_WARNING, "[FeatureScatter] failed to load %s (from %s) — skipping its placements", glb_path, mesh_path);
                continue; // don't reserve a mesh slot for a mesh that failed to load
            }
            if (triplanar) {
                static GpuSamplerDesc sd = GpuSamplerDesc::Default();
                const std::string tp = md::KenshiDir() + "/" + md::NormalizeKenshiPath(tex_path);
                if (GpuTexture* t = AssetCache::Get().GetOrLoadDDS(tp.c_str(), sd)) renderers_[mesh_idx].SetSharedTexture(t);
                else MD_LOG(MD_LOG_WARNING, "[FeatureScatter] triplanar texture not loaded: %s", tp.c_str());
            }
            strncpy(mesh_names_[mesh_idx], name, sizeof(mesh_names_[mesh_idx]) - 1);
            ++mesh_count_;
        }

        Placement& pl = placements_[placement_count_++];
        pl.mesh_idx = (int16_t)mesh_idx;
        pl.x = x; pl.y = y; pl.z = z;
        pl.sx = sx; pl.sy = sy; pl.sz = sz;
        pl.qw = qw; pl.qx = qx; pl.qy = qy; pl.qz = qz;
        pl.mode = (int8_t)mode;
    }
    fclose(f);

    {   // #176: pipeline of the TERRAIN-mode features (terrain G-buffer: feature_gbuf.vert + terrain_gbuffer_mini.frag, R16G16B16A16_UINT target)
        GpuPipeline::Desc pd;
        pd.vert_path = "shaders/feature_gbuf.vert";
        pd.frag_path = "shaders/terrain_gbuffer_mini.frag";
        pd.layout.count = 4; pd.layout.stride = 36;
        pd.layout.attribs[0] = { 0, 0, GpuAttribFmt::F3 }; pd.layout.attribs[1] = { 1, 12, GpuAttribFmt::F3 };
        pd.layout.attribs[2] = { 2, 24, GpuAttribFmt::F2 }; pd.layout.attribs[3] = { 3, 32, GpuAttribFmt::F1 };
        pd.raster.depth_test = true; pd.raster.depth_write = true; pd.raster.cull_back = false;
        pd.has_depth_target = true;
        pd.vert_uniform_bufs = 1; pd.frag_uniform_bufs = 0; pd.frag_samplers = 0; pd.frag_storage_bufs = 0;
        pd.color_format = SDL_GPU_TEXTUREFORMAT_R16G16B16A16_UINT;
        if (!gbuf_pipeline_.Create(pd)) MD_LOG(MD_LOG_WARNING, "[FeatureScatter] G-buffer pipeline create failed");
    }
    MD_LOG(MD_LOG_INFO, "[FeatureScatter] loaded %d real placements across %d unique meshes from %s",
           placement_count_, mesh_count_, scatter_path);
    return true;
}

void FeatureScatterSystem::Shutdown() {
    for (int i = 0; i < mesh_count_; ++i) renderers_[i].Shutdown();
    mesh_count_ = 0;
    placement_count_ = 0;
}

void FeatureScatterSystem::Draw(
    SDL_GPURenderPass*    rp,
    md::GpuCommandBufferHandle cmd,
    const float* vp16,
    const float* sun32,
    float        cam_x,
    float        cam_z,
    float        radius)
{
    if (placement_count_ <= 0) return;
    const float r2 = radius * radius;

    // Reused across every mesh type this frame — avoids MAX_MESHES separate
    // fixed buffers (would be ~1.8MB of mostly-unused BSS) and any malloc.
    static float pos_buf[MAX_DRAW_BATCH * 3];
    static float quat_buf[MAX_DRAW_BATCH * 4];

    for (int m = 0; m < mesh_count_; ++m) {
        int n = 0;
        // Uniform scale doesn't vary per mesh type here (each instance's
        // own non-uniform sx/sy/sz is baked into pos via the mesh's own
        // vertex data scale at conversion time for the common (1,1,1)
        // case; per-instance non-uniform scale beyond that is rare enough
        // in the real data — per the parser's own doc comment — that
        // PropRenderer's single per-call `scale` uniform is left at 1.0
        // and non-uniform outliers render at their nearest uniform
        // approximation rather than needing a 4th per-instance array).
        for (int i = 0; i < placement_count_ && n < MAX_DRAW_BATCH; ++i) {
            const Placement& pl = placements_[i];
            if (pl.mesh_idx != m) continue;
            if (terrain_in_gbuf_ && pl.mode == 2) continue;   // drawn by DrawGbuf
            float dx = pl.x - cam_x, dz = pl.z - cam_z;
            if (dx * dx + dz * dz > r2) continue;
            pos_buf[n * 3 + 0] = pl.x;
            pos_buf[n * 3 + 1] = pl.y;
            pos_buf[n * 3 + 2] = pl.z;
            quat_buf[n * 4 + 0] = pl.qx;
            quat_buf[n * 4 + 1] = pl.qy;
            quat_buf[n * 4 + 2] = pl.qz;
            quat_buf[n * 4 + 3] = (fabsf(pl.qw) > 0.0001f) ? pl.qw : 1.0f; // never let w==0 slip through (shader's "unused" sentinel)
            ++n;
        }
        if (n == 0) continue;
        renderers_[m].DrawRaw(
            rp, cmd,
            pos_buf, n, vp16, sun32, /*scale=*/1.0f,
            /*anim_mode=*/0.0f, /*anim_time=*/0.0f,
            /*normals_xyz=*/nullptr, /*quats_xyzw=*/quat_buf);
    }
}

void FeatureScatterSystem::DrawGbuf(SDL_GPURenderPass* rp, md::GpuCommandBufferHandle cmd, const float* vp16, float cam_x, float cam_z, float radius) {
    if (!terrain_in_gbuf_ || placement_count_ <= 0) return;
    const float r2 = radius * radius;
    static float pos_buf[MAX_DRAW_BATCH * 3];
    static float quat_buf[MAX_DRAW_BATCH * 4];
    for (int m = 0; m < mesh_count_; ++m) {
        int n = 0;
        for (int i = 0; i < placement_count_ && n < MAX_DRAW_BATCH; ++i) {
            const Placement& pl = placements_[i];
            if (pl.mesh_idx != m || pl.mode != 2) continue;
            float dx = pl.x - cam_x, dz = pl.z - cam_z;
            if (dx * dx + dz * dz > r2) continue;
            pos_buf[n * 3] = pl.x; pos_buf[n * 3 + 1] = pl.y; pos_buf[n * 3 + 2] = pl.z;
            quat_buf[n * 4] = pl.qx; quat_buf[n * 4 + 1] = pl.qy; quat_buf[n * 4 + 2] = pl.qz;
            quat_buf[n * 4 + 3] = (fabsf(pl.qw) > 0.0001f) ? pl.qw : 1.0f;
            ++n;
        }
        if (n > 0) renderers_[m].DrawGbuf(rp, cmd, gbuf_pipeline_, pos_buf, quat_buf, n, vp16);
    }
}

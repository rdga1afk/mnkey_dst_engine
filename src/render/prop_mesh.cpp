// PropMesh — GLB loader using cgltf (CGLTF_IMPLEMENTATION in cgltf_impl.cpp).
// Only the first mesh primitive is loaded; positions + normals are extracted.
#include <monkey_dust/render/prop_mesh.h>

// cgltf.h lives in the same directory as this .cpp.
// CGLTF_IMPLEMENTATION is provided by cgltf_impl.cpp (compiled separately).
#include "cgltf.h"
// STB_IMAGE_IMPLEMENTATION lives in stb_image_impl.cpp (compiled separately,
// same pattern as gpu_hal_buffers.cpp / rd_texture.cpp's existing includes).
#include "stb_image.h"

#include <monkey_dust/io/kenshi_dir.h>
#include <monkey_dust/io/kenshi_resources.h>
#include <monkey_dust/io/ogre_mesh.h>
#include <filesystem>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

bool LoadOgreStaticMesh(const std::string& stored, float scale, float layer, std::vector<PropVertex>& verts, std::vector<uint32_t>& indices, bool keep_uv) {
    const std::string root = md::KenshiDir();
    if (root.empty()) return false;
    std::string file = root + "/" + md::NormalizeKenshiPath(stored);
    if (!std::filesystem::exists(file)) {   // Ogre finds a mesh by file name anywhere under data/
        static md::KenshiResources res; static bool indexed = false;
        if (!indexed) { res.Index(root); indexed = true; }
        file = res.Path(stored);
        if (file.empty()) return false;
    }
    md::ogre::Mesh m; md::ogre::Error err;
    if (!md::ogre::ReadMesh(file, m, err)) return false;
    verts.clear(); indices.clear();
    auto append = [&](const md::ogre::Geometry& g) {
        std::vector<float> p, n, uv;
        g.Positions(p);
        const bool hn = g.Normals(n), hu = keep_uv && g.Uvs(0, uv);
        for (uint32_t i = 0; i < g.vertex_count; ++i) {
            PropVertex v{};
            v.x = (float)((double)p[i * 3] * scale); v.y = (float)((double)p[i * 3 + 1] * scale); v.z = (float)((double)p[i * 3 + 2] * scale);
            v.nx = hn ? n[i * 3] : 0.f; v.ny = hn ? n[i * 3 + 1] : 1.f; v.nz = hn ? n[i * 3 + 2] : 0.f;
            v.u = hu ? uv[i * 2] : 0.f; v.v = hu ? uv[i * 2 + 1] : 0.f; v.layer = layer;
            verts.push_back(v);
        }
    };
    uint32_t shared_base = 0; bool shared_added = false;
    for (const md::ogre::SubMesh& s : m.submeshes) {
        uint32_t base;
        if (s.use_shared) {
            if (!m.has_shared) continue;
            if (!shared_added) { shared_base = (uint32_t)verts.size(); append(m.shared); shared_added = true; }
            base = shared_base;
        } else {
            if (!s.has_geometry) continue;
            base = (uint32_t)verts.size(); append(s.geometry);
        }
        for (uint32_t i : s.indices) indices.push_back(base + i);
    }
    return !verts.empty() && !indices.empty();
}

bool PropMesh::LoadGLB(const char* path, float layer) {
    loaded = false;
    if (!path) return false;

    cgltf_options opts = {};
    cgltf_data*   data = nullptr;

    if (cgltf_parse_file(&opts, path, &data) != cgltf_result_success) {
        fprintf(stderr, "[PropMesh] cgltf_parse_file failed: %s\n", path);
        return false;
    }

    if (cgltf_load_buffers(&opts, data, path) != cgltf_result_success) {
        fprintf(stderr, "[PropMesh] cgltf_load_buffers failed: %s\n", path);
        cgltf_free(data);
        return false;
    }

    // Locate first mesh primitive with POSITION + NORMAL.
    cgltf_primitive* prim    = nullptr;
    cgltf_mesh*      own_mesh = nullptr;
    for (cgltf_size mi = 0; mi < data->meshes_count && !prim; ++mi) {
        for (cgltf_size pi = 0; pi < data->meshes[mi].primitives_count && !prim; ++pi) {
            cgltf_primitive* p = &data->meshes[mi].primitives[pi];
            if (p->type != cgltf_primitive_type_triangles) continue;
            bool has_pos = false, has_norm = false;
            for (cgltf_size ai = 0; ai < p->attributes_count; ++ai) {
                if (p->attributes[ai].type == cgltf_attribute_type_position) has_pos  = true;
                if (p->attributes[ai].type == cgltf_attribute_type_normal)   has_norm = true;
            }
            if (has_pos && has_norm && p->indices) { prim = p; own_mesh = &data->meshes[mi]; }
        }
    }

    if (!prim) {
        fprintf(stderr, "[PropMesh] No valid primitive with POSITION+NORMAL+indices: %s\n", path);
        cgltf_free(data);
        return false;
    }

    // task gltfpack-node-transform (2026-08-02): a plain Blender export bakes
    // object transform into vertex data directly (transform_apply before
    // export), so historically every GLB this loader saw had an implicit
    // identity node transform and skipping data->nodes[] here never mattered.
    // gltfpack's default KHR_mesh_quantization output does NOT do this -- it
    // relies on a node-level scale/translate to map quantized integers back
    // to real units, so a loader that ignores node transforms renders
    // grossly wrong geometry (confirmed live: a 1x1x0.1m tile became a huge
    // distorted shape). Find the node owning this mesh and apply its WORLD
    // transform (cgltf_node_transform_world composes the full ancestor
    // chain) to every position/normal below. Identity for GLBs with no
    // matching node (world matrix defaults to identity either way).
    float world_m[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
    for (cgltf_size ni = 0; ni < data->nodes_count; ++ni) {
        if (data->nodes[ni].mesh == own_mesh) {
            cgltf_node_transform_world(&data->nodes[ni], world_m);
            break;
        }
    }
    bool has_node_transform = false;
    for (int i = 0; i < 16; ++i) {
        float expect = (i == 0 || i == 5 || i == 10 || i == 15) ? 1.0f : 0.0f;
        if (world_m[i] != expect) { has_node_transform = true; break; }
    }

    // task propmesh-materials (2026-08-02): this loader previously never
    // read the primitive's material at all -- PropRenderer always shaded
    // with one of two shared hardcoded textures (PropTexShared's
    // tex_rock/tex_veg, selected by the caller-supplied `layer` param),
    // silently ignoring whatever real per-mesh diffuse texture a GLB
    // actually embedded (asset pipeline Phase 2/4 finding, see
    // tools/kenshi_import/CONVENTIONS.md). Decode the primitive's
    // pbr_metallic_roughness base-color image here when present, so a
    // real Kenshi-textured GLB (blender_convert.py's output) can light up
    // with its own texture instead of the rock/veg fallback -- overriding
    // `layer` to 2.0 to select it in prop.frag. Blender's GLB exporter
    // embeds images into the binary chunk (image->buffer_view), not an
    // external/data-uri (image->uri) -- only that path is handled; an
    // external-URI image is left unsupported (has_custom_tex stays false,
    // falls back to the old rock/veg behavior, same as before this change).
    float    effective_layer = layer;
    uint8_t* decoded_rgba    = nullptr;
    int      decoded_w = 0, decoded_h = 0;
    if (prim->material && prim->material->has_pbr_metallic_roughness) {
        cgltf_texture_view* bctv = &prim->material->pbr_metallic_roughness.base_color_texture;
        if (bctv->texture && bctv->texture->image) {
            cgltf_image* img = bctv->texture->image;
            if (img->buffer_view && img->buffer_view->buffer &&
                img->buffer_view->buffer->data) {
                const uint8_t* bytes = (const uint8_t*)img->buffer_view->buffer->data
                                        + img->buffer_view->offset;
                int comp = 0;
                decoded_rgba = stbi_load_from_memory(bytes, (int)img->buffer_view->size,
                                                      &decoded_w, &decoded_h, &comp, 4);
                if (decoded_rgba) effective_layer = 2.0f;
            }
        }
    }

    // Find POSITION, NORMAL and (optional) TEXCOORD_0 accessors.
    cgltf_accessor* pos_acc  = nullptr;
    cgltf_accessor* norm_acc = nullptr;
    cgltf_accessor* uv_acc   = nullptr;
    for (cgltf_size ai = 0; ai < prim->attributes_count; ++ai) {
        if (prim->attributes[ai].type == cgltf_attribute_type_position) pos_acc  = prim->attributes[ai].data;
        if (prim->attributes[ai].type == cgltf_attribute_type_normal)   norm_acc = prim->attributes[ai].data;
        if (prim->attributes[ai].type == cgltf_attribute_type_texcoord) uv_acc   = prim->attributes[ai].data;
    }

    cgltf_size vert_count = pos_acc->count;
    if (vert_count == 0 || vert_count > 131072) {
        fprintf(stderr, "[PropMesh] Unexpected vertex count %zu: %s\n", vert_count, path);
        if (decoded_rgba) stbi_image_free(decoded_rgba);
        cgltf_free(data);
        return false;
    }

    // Build interleaved VBO: PropVertex { pos(12) + norm(12) + uv(8) + layer(4) }.
    static PropVertex s_verts[131072];
    float y_min =  1e30f, y_max = -1e30f;
    for (cgltf_size i = 0; i < vert_count; ++i) {
        float p[3]  = {0.f, 0.f, 0.f};
        float n[3]  = {0.f, 1.f, 0.f};
        float uv[2] = {0.f, 0.f};
        cgltf_accessor_read_float(pos_acc,  i, p, 3);
        cgltf_accessor_read_float(norm_acc, i, n, 3);
        if (uv_acc) cgltf_accessor_read_float(uv_acc, i, uv, 2);

        if (has_node_transform) {
            float wp[3], wn[3];
            wp[0] = world_m[0]*p[0] + world_m[4]*p[1] + world_m[8]*p[2]  + world_m[12];
            wp[1] = world_m[1]*p[0] + world_m[5]*p[1] + world_m[9]*p[2]  + world_m[13];
            wp[2] = world_m[2]*p[0] + world_m[6]*p[1] + world_m[10]*p[2] + world_m[14];
            // 3x3 part only for normals (no translation); adequate for the
            // uniform-scale+translate transforms gltfpack's quantization
            // dequant node emits -- not a full inverse-transpose, so a
            // future non-uniform-scale source would need revisiting this.
            wn[0] = world_m[0]*n[0] + world_m[4]*n[1] + world_m[8]*n[2];
            wn[1] = world_m[1]*n[0] + world_m[5]*n[1] + world_m[9]*n[2];
            wn[2] = world_m[2]*n[0] + world_m[6]*n[1] + world_m[10]*n[2];
            float nlen = sqrtf(wn[0]*wn[0] + wn[1]*wn[1] + wn[2]*wn[2]);
            if (nlen > 1e-8f) { wn[0] /= nlen; wn[1] /= nlen; wn[2] /= nlen; }
            p[0] = wp[0]; p[1] = wp[1]; p[2] = wp[2];
            n[0] = wn[0]; n[1] = wn[1]; n[2] = wn[2];
        }

        s_verts[i] = { p[0], p[1], p[2], n[0], n[1], n[2], uv[0], uv[1], effective_layer };
        if (p[1] < y_min) y_min = p[1];
        if (p[1] > y_max) y_max = p[1];
    }
    aabb_y_min = y_min;
    aabb_y_max = y_max;

    vbo.Init(0x8892u /*GL_ARRAY_BUFFER*/,
             s_verts,
             (uint32_t)(vert_count * sizeof(PropVertex)));

    // Build IBO: uint16_t if vert_count <= 65535, else uint32_t.
    cgltf_accessor* idx_acc = prim->indices;
    cgltf_size      idx_count = idx_acc->count;

    indices_u16 = (vert_count <= 65535u);
    index_count = (uint32_t)idx_count;

    if (indices_u16) {
        static uint16_t s_idx16[1024 * 1024];
        for (cgltf_size i = 0; i < idx_count; ++i)
            s_idx16[i] = (uint16_t)cgltf_accessor_read_index(idx_acc, i);
        ibo.Init(0x8893u /*GL_ELEMENT_ARRAY_BUFFER*/,
                 s_idx16,
                 (uint32_t)(idx_count * sizeof(uint16_t)));
    } else {
        static uint32_t s_idx32[1024 * 1024];
        for (cgltf_size i = 0; i < idx_count; ++i)
            s_idx32[i] = (uint32_t)cgltf_accessor_read_index(idx_acc, i);
        ibo.Init(0x8893u /*GL_ELEMENT_ARRAY_BUFFER*/,
                 s_idx32,
                 (uint32_t)(idx_count * sizeof(uint32_t)));
    }

    cgltf_free(data);
    loaded = true;
    if (decoded_rgba) {
        has_custom_tex  = true;
        custom_tex_rgba = decoded_rgba;
        custom_tex_w    = decoded_w;
        custom_tex_h    = decoded_h;
    }
    fprintf(stdout, "[PropMesh] Loaded %zu verts / %zu idx (u%s)%s: %s\n",
            vert_count, idx_count, indices_u16 ? "16" : "32",
            has_custom_tex ? " +custom_tex" : "", path);
    return true;
}

bool PropMesh::LoadFromArrays(const PropVertex* verts, uint32_t vert_count, const uint32_t* idx, uint32_t idx_count) {
    loaded = false;
    if (!verts || !idx || vert_count == 0 || idx_count == 0) return false;
    float y_min = verts[0].y, y_max = verts[0].y;
    for (uint32_t i = 1; i < vert_count; ++i) {
        if (verts[i].y < y_min) y_min = verts[i].y;
        if (verts[i].y > y_max) y_max = verts[i].y;
    }
    aabb_y_min = y_min;
    aabb_y_max = y_max;
    vbo.Init(0x8892u, verts, vert_count * (uint32_t)sizeof(PropVertex));
    indices_u16 = (vert_count <= 65535u);
    index_count = idx_count;
    if (indices_u16) {
        uint16_t* tmp = new uint16_t[idx_count];
        for (uint32_t i = 0; i < idx_count; ++i) tmp[i] = (uint16_t)idx[i];
        ibo.Init(0x8893u, tmp, idx_count * (uint32_t)sizeof(uint16_t));
        delete[] tmp;
    } else {
        ibo.Init(0x8893u, idx, idx_count * (uint32_t)sizeof(uint32_t));
    }
    loaded = true;
    return true;
}

void PropMesh::Shutdown() {
    vbo.Shutdown();
    ibo.Shutdown();
    index_count = 0;
    loaded      = false;
    if (custom_tex_rgba) { stbi_image_free(custom_tex_rgba); custom_tex_rgba = nullptr; }
    has_custom_tex = false;
}

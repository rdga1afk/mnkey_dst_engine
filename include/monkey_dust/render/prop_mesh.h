#pragma once
// PropMesh — loads a GLB file (first mesh primitive) into GPU static buffers.
// Vertex layout: PropVertex { float x,y,z, nx,ny,nz, u,v, layer; } — stride=36 bytes.
// Index format: uint16_t if vertex count <= 65535, else uint32_t.
// Usage: Init once; Draw* calls reference vbo/ibo; Shutdown on exit.
#include <monkey_dust/render/gpu_static_buffer.h>
#include <cstdint>

struct PropVertex {
    float x, y, z;    // position
    float nx, ny, nz; // normal
    float u, v;       // texture coords (TEXCOORD_0 baked in the source Kenshi mesh)
    float layer;      // PropTexShared layer: 0=rock diffuse, 1=vegetation atlas, 2=custom (PropMesh::has_custom_tex)
};
static_assert(sizeof(PropVertex) == 36, "PropVertex stride mismatch");

class PropMesh {
public:
    // Load GLB from path.  Returns false and sets loaded=false on failure.
    // Passing nullptr as path is a no-op — returns false immediately.
    // layer: written into every vertex — see PropVertex::layer / PropTexShared.
    // Overridden to 2.0 (real per-mesh texture) automatically when the GLB's
    // primitive has a decodable embedded base-color texture — see
    // has_custom_tex / custom_tex_rgba below.
    bool LoadGLB(const char* path, float layer = 0.0f);
    void Shutdown();

    GpuStaticBuffer vbo;
    GpuStaticBuffer ibo;
    uint32_t        index_count = 0;
    bool            loaded      = false;
    bool            indices_u16 = true;
    float           aabb_y_min  = 0.0f;  // model-space Y extent (for animation)
    float           aabb_y_max  = 1.0f;

    // Real per-mesh diffuse texture, decoded from the primitive material's
    // embedded base-color image (asset pipeline Phase 2/4 gap — see
    // tools/kenshi_import/CONVENTIONS.md "PropMesh does not read GLB
    // materials" finding). Ownership: stb_image-allocated, caller (e.g.
    // PropRenderer::Init) must stbi_image_free() this after uploading to
    // GPU, or LoadGLB()/Shutdown() frees it if never consumed.
    bool     has_custom_tex   = false;
    uint8_t* custom_tex_rgba  = nullptr;
    int      custom_tex_w     = 0;
    int      custom_tex_h     = 0;
};

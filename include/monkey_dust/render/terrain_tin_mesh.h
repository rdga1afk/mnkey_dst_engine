#pragma once
#ifdef MD_SDL_GPU
#include <monkey_dust/render/gpu_static_buffer.h>
#include <monkey_dust/render/gpu_device.h>
#include <cstdint>

// TIN Etap 2 Stage 1 (docs/TIN_ETAP2_PLAN.md): loads a single zone's baked
// Delaunay/TIN mesh (tools/md_bake_tin_terrain.py's binary output) into GPU
// static buffers. Vertex layout mirrors PropMesh's real-vertex-buffer
// convention (pos+normal interleaved, stride 24 bytes) -- NOT
// TerrainQuadtreeRenderer's layout.count=0 procedural/VTF path, since TIN
// vertices are irregular by construction and must be baked into a real
// buffer (see TIN_ETAP2_PLAN.md's own "structural change" note).
//
// File format (little-endian, written by md_bake_tin_terrain.py):
//   char     magic[4]        "MDTN"
//   uint32_t version         1
//   int32_t  zone_x, zone_z
//   uint32_t vertex_count, index_count
//   TerrainTinVertex[vertex_count]   pos.xyz (zone-local metres) + normal.xyz
//   uint32_t[index_count]
//
// Presence-gated load (docs/TIN_ETAP2_PLAN.md's scope decision: no MD5/
// hash cache, same convention as game/data/terrain_detail_baked/) -- Init()
// returns false and leaves the mesh !IsReady() if the file doesn't exist
// yet (bake hasn't been run for this zone), same tolerance
// TerrainSkyVisAO's own Init() failure path already uses.
struct TerrainTinVertex {
    float x, y, z;    // position, zone-local metres (0..CHUNK_SIZE_M on X/Z)
    float nx, ny, nz; // normal
};
static_assert(sizeof(TerrainTinVertex) == 24, "TerrainTinVertex stride mismatch");

class TerrainTinMesh {
public:
    bool Init(md::GpuDeviceHandle dev, const char* path);
    void Shutdown();
    bool IsReady() const { return ready_; }

    GpuStaticBuffer vbo;
    GpuStaticBuffer ibo;
    uint32_t        index_count = 0;
    int             zone_x = 0, zone_z = 0;

private:
    bool ready_ = false;
};
#endif

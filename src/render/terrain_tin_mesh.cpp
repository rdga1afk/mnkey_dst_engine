#ifdef MD_SDL_GPU
#include <monkey_dust/render/terrain_tin_mesh.h>
#include <monkey_dust/platform/md_log.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {
struct FileHeader {
    char     magic[4];
    uint32_t version;
    int32_t  zone_x, zone_z;
    uint32_t vertex_count, index_count;
};
} // namespace

bool TerrainTinMesh::Init(md::GpuDeviceHandle dev, const char* path) {
    (void)dev;
    if (!path) return false;
    FILE* f = fopen(path, "rb");
    if (!f) {
        MD_LOG(MD_LOG_INFO, "[TerrainTinMesh] %s not found (bake not run yet)", path);
        return false;
    }

    FileHeader hdr{};
    if (fread(&hdr, sizeof(hdr), 1, f) != 1 || memcmp(hdr.magic, "MDTN", 4) != 0) {
        MD_LOG(MD_LOG_WARNING, "[TerrainTinMesh] %s bad header", path);
        fclose(f);
        return false;
    }
    if (hdr.vertex_count == 0 || hdr.index_count == 0) {
        MD_LOG(MD_LOG_WARNING, "[TerrainTinMesh] %s empty mesh", path);
        fclose(f);
        return false;
    }

    size_t vbytes = (size_t)hdr.vertex_count * sizeof(TerrainTinVertex);
    size_t ibytes = (size_t)hdr.index_count * sizeof(uint32_t);
    TerrainTinVertex* verts = (TerrainTinVertex*)malloc(vbytes);
    uint32_t* indices = (uint32_t*)malloc(ibytes);
    if (!verts || !indices) {
        MD_LOG(MD_LOG_WARNING, "[TerrainTinMesh] %s alloc failed", path);
        free(verts); free(indices);
        fclose(f);
        return false;
    }
    bool ok = fread(verts, 1, vbytes, f) == vbytes &&
              fread(indices, 1, ibytes, f) == ibytes;
    fclose(f);
    if (!ok) {
        MD_LOG(MD_LOG_WARNING, "[TerrainTinMesh] %s truncated", path);
        free(verts); free(indices);
        return false;
    }

    vbo.Init(0x8892u /*GL_ARRAY_BUFFER*/, verts, (uint32_t)vbytes);
    ibo.Init(0x8893u /*GL_ELEMENT_ARRAY_BUFFER*/, indices, (uint32_t)ibytes);
    free(verts);
    free(indices);

    index_count = hdr.index_count;
    zone_x = hdr.zone_x;
    zone_z = hdr.zone_z;
    ready_ = true;
    MD_LOG(MD_LOG_INFO, "[TerrainTinMesh] %s: zone(%d,%d) %u verts, %u indices",
           path, zone_x, zone_z, hdr.vertex_count, hdr.index_count);
    return true;
}

void TerrainTinMesh::Shutdown() {
    vbo.Shutdown();
    ibo.Shutdown();
    ready_ = false;
    index_count = 0;
}
#endif

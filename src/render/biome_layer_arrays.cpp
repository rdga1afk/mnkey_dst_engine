#include <monkey_dust/render/biome_layer_arrays.h>
#include <monkey_dust/world/biome_def.h>
#include <cstdio>
#include <filesystem>

namespace md {

// key = biome * 8 + tier, tier = skipped mips / 2 (0, 2, 4, 6 dropped levels)
static BiomeLayerCache::Built BuildBiome(int key) {
    const int biome = key / 8, skip = (key % 8) * 2;
    const BiomeRegistry& R = BiomeRegistry::Get();
    if (biome < 0 || biome >= R.BiomeCount()) return {};
    const BiomeDef& d = R.ForIndex(biome);
    const int tex[6] = { d.tex_base, d.tex_slope, d.tex_cliff, d.tex_grass, d.tex_dirt, d.tex_road };
    const char* dif[6]; const char* nml[6]; uint64_t bytes = 0;
    for (int i = 0; i < 6; ++i) {
        dif[i] = R.GroundTexPath(tex[i]); nml[i] = R.GroundNmlPath(tex[i]);
        if (!dif[i] || !*dif[i] || !nml[i] || !*nml[i]) return {};
        std::error_code ec;
        bytes += (std::filesystem::file_size(dif[i], ec) + std::filesystem::file_size(nml[i], ec)) >> (2 * skip);
    }
    GpuSamplerDesc sd;
    sd.min_filter = GpuSamplerDesc::Filter::LINEAR_MIPMAP; sd.mag_filter = GpuSamplerDesc::Filter::LINEAR;
    sd.wrap_s = GpuSamplerDesc::Wrap::REPEAT; sd.wrap_t = GpuSamplerDesc::Wrap::REPEAT; sd.gen_mipmap = false;
    auto* set = new BiomeLayerSet;
    if (!set->diffuse.InitFromDDSArray(dif, 6, sd, skip) || !set->normal.InitFromDDSArray(nml, 6, sd, skip)) {
        fprintf(stderr, "[BiomeLayerArrays] biome %d (skip %d mips): layer arrays failed\n", biome, skip);
        delete set; return {};
    }
    return { set, bytes };
}

BiomeLayerArrays::BiomeLayerArrays()
    : cache_(BuildBiome, [](void* h) { delete static_cast<BiomeLayerSet*>(h); }) {}

}   // namespace md

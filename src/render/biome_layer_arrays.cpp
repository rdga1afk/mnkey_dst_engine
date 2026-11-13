#include <monkey_dust/render/biome_layer_arrays.h>
#include <monkey_dust/world/biome_def.h>
#include <chrono>
#include <cstdio>
#include <filesystem>

namespace md {

// The textures are released here (a deleted set alone keeps its GPU textures: GpuTexture has no destructor); only while the device lives.
static void FreeSet(BiomeLayerSet* set) { if (!set) return; set->diffuse.Shutdown(); set->normal.Shutdown(); delete set; }

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
        FreeSet(set); return {};
    }
    return { set, bytes };
}

BiomeLayerArrays::BiomeLayerArrays()
    : cache_(BuildBiome, [](void* h) { FreeSet(static_cast<BiomeLayerSet*>(h)); }) {}

void BiomeLayerArrays::Stop() {
    { std::lock_guard<std::mutex> l(mu_); stop_ = true; }
    cv_.notify_all();
    if (worker_.joinable()) worker_.join();
    for (Done& d : done_) FreeSet(d.set);
    done_.clear(); cache_.Clear();
}

BiomeLayerArrays::~BiomeLayerArrays() { Stop(); }

void BiomeLayerArrays::Request(int biome, int tier) {
    const int key = biome * 8 + tier;
    std::lock_guard<std::mutex> l(mu_);
    for (int k : known_) if (k == key) return;
    known_.push_back(key); queue_.push_back(key);
    if (stop_) return;
    if (!running_) { running_ = true; worker_ = std::thread([this] { Worker(); }); }
    cv_.notify_one();
}

void BiomeLayerArrays::Worker() {
    SetDdsArrayNoJobsThisThread(true);
    for (;;) {
        int key;
        { std::unique_lock<std::mutex> l(mu_); cv_.wait(l, [&] { return stop_ || !queue_.empty(); }); if (stop_) return; key = queue_.front(); queue_.pop_front(); ++in_flight_; }
        const BiomeLayerCache::Built b = BuildBiome(key);   // the whole synchronous build, off the render thread
        { std::lock_guard<std::mutex> l(mu_); --in_flight_; if (b.handle) done_.push_back({ key, static_cast<BiomeLayerSet*>(b.handle), b.bytes }); }
        if (gap_ms_ > 0) std::this_thread::sleep_for(std::chrono::milliseconds(gap_ms_));   // spread the uploads: the integrated GPU shares the memory bus with the copy
    }
}

void BiomeLayerArrays::Poll(uint64_t frame) {
    std::vector<Done> d;
    { std::lock_guard<std::mutex> l(mu_); d.swap(done_); }
    for (Done& x : d) cache_.Insert(x.key, x.set, x.bytes, frame);
    if (!d.empty()) { std::lock_guard<std::mutex> l(mu_); for (Done& x : d) for (size_t i = 0; i < known_.size(); ++i) if (known_[i] == x.key) { known_.erase(known_.begin() + (long)i); break; } }   // resident now; a failed key stays known
}

int BiomeLayerArrays::Pending() const { std::lock_guard<std::mutex> l(mu_); return (int)queue_.size() + in_flight_; }

}   // namespace md

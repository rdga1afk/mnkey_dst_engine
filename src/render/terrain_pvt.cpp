#include <monkey_dust/render/terrain_pvt.h>
#include <monkey_dust/render/gpu_hal_free_functions.h>
#include <monkey_dust/platform/md_log.h>
#include <algorithm>
#include <cmath>
#include <cstring>

bool TerrainPvt::Init(SDL_GPUDevice* dev, int megabytes) {
    layers_ = std::max(16, std::min(2000, megabytes * 1024 / 96));   // 96 KB per tile: 128 x 128 x (4 + 2) bytes
    SDL_GPUTextureCreateInfo ci{};
    ci.type = SDL_GPU_TEXTURETYPE_2D_ARRAY; ci.width = kTexels; ci.height = kTexels; ci.layer_count_or_depth = (Uint32)layers_; ci.num_levels = 1;
    ci.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COLOR_TARGET;
    ci.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM_SRGB;
    albedo_ = GpuCreateTexture(dev, &ci);
    ci.format = SDL_GPU_TEXTUREFORMAT_R8G8_UNORM;
    normal_ = GpuCreateTexture(dev, &ci);
    SDL_GPUTextureCreateInfo ti{};
    ti.type = SDL_GPU_TEXTURETYPE_2D; ti.format = SDL_GPU_TEXTUREFORMAT_R16G16B16A16_UINT; ti.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
    ti.width = kRing; ti.height = kRing * kMips; ti.layer_count_or_depth = 1; ti.num_levels = 1;
    table_ = GpuCreateTexture(dev, &ti);
    SDL_GPUSamplerCreateInfo si{};
    si.min_filter = si.mag_filter = SDL_GPU_FILTER_LINEAR; si.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
    si.address_mode_u = si.address_mode_v = si.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    lin_ = GpuCreateSampler(dev, &si);
    si.min_filter = si.mag_filter = SDL_GPU_FILTER_NEAREST;
    nearest_ = GpuCreateSampler(dev, &si);
    SDL_GPUTransferBufferCreateInfo tbi{};
    tbi.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD; tbi.size = (Uint32)(kRing * kRing * kMips * sizeof(Entry));
    tb_ = SDL_CreateGPUTransferBuffer(dev, &tbi);
    if (!albedo_ || !normal_ || !table_ || !lin_ || !nearest_ || !tb_) {
        MD_LOG(MD_LOG_WARNING, "[TerrainPvt] resource create failed: %s", SDL_GetError());
        Shutdown(dev);
        return false;
    }
    table_cpu_ = new Entry[kMips * kRing * kRing];
    layer_slot_ = new int32_t[layers_]; layer_seen_ = new uint32_t[layers_]; free_ = new int32_t[layers_];
    Flush();
    MD_LOG(MD_LOG_INFO, "[TerrainPvt] atlas %d layers of %dx%d (%d MB), page table %dx%d x %d mips", layers_, kTexels, kTexels, layers_ * 96 / 1024, kRing, kRing, kMips);
    return true;
}

void TerrainPvt::Shutdown(SDL_GPUDevice* dev) {
    if (albedo_) GpuReleaseTexture(dev, albedo_);
    if (normal_) GpuReleaseTexture(dev, normal_);
    if (table_)  GpuReleaseTexture(dev, table_);
    if (lin_)     GpuReleaseSampler(dev, lin_);
    if (nearest_) GpuReleaseSampler(dev, nearest_);
    if (tb_) SDL_ReleaseGPUTransferBuffer(dev, tb_);
    albedo_ = normal_ = table_ = nullptr; lin_ = nearest_ = nullptr; tb_ = nullptr;
    delete[] table_cpu_; delete[] layer_slot_; delete[] layer_seen_; delete[] free_;
    table_cpu_ = nullptr; layer_slot_ = nullptr; layer_seen_ = nullptr; free_ = nullptr;
}

void TerrainPvt::Flush() {
    if (!table_cpu_) return;
    std::memset(table_cpu_, 0, sizeof(Entry) * kMips * kRing * kRing);
    for (int l = 0; l < layers_; ++l) { layer_slot_[l] = -1; layer_seen_[l] = 0; free_[l] = layers_ - 1 - l; }
    free_top_ = layers_; resident_ = 0; dirty_ = true;
}

void TerrainPvt::UploadTable(SDL_GPUDevice* dev, SDL_GPUCommandBuffer* cmd) {
    if (!dirty_ || !Ready()) return;
    const Uint32 bytes = (Uint32)(kRing * kRing * kMips * sizeof(Entry));
    void* p = SDL_MapGPUTransferBuffer(dev, tb_, true);
    if (!p) return;
    std::memcpy(p, table_cpu_, bytes);
    SDL_UnmapGPUTransferBuffer(dev, tb_);
    SDL_GPUCopyPass* cp = SDL_BeginGPUCopyPass(cmd);
    SDL_GPUTextureTransferInfo src{}; src.transfer_buffer = tb_; src.offset = 0; src.pixels_per_row = kRing; src.rows_per_layer = kRing * kMips;
    SDL_GPUTextureRegion dst{}; dst.texture = table_; dst.w = kRing; dst.h = kRing * kMips; dst.d = 1;
    SDL_UploadToGPUTexture(cp, &src, &dst, false);
    SDL_EndGPUCopyPass(cp);
    dirty_ = false;
}

namespace {
struct Want { int mip, ti, tj; float dist; };
constexpr int kMaxWant = 4096;
uint32_t HashKey(int mip, int ti, int tj) { return ((uint32_t)mip * 0x9E3779B1u) ^ ((uint32_t)ti * 0x85EBCA6Bu) ^ ((uint32_t)tj * 0xC2B2AE35u); }
}

int TerrainPvt::Plan(const float cam[3], const float inv_vp[16], float pix_angle, HeightFn hf, const float fine_min[2], const float fine_max[2], uint32_t frame, Bake* out, int max_out) {
    if (!Ready() || !hf || pix_angle <= 0.f) return 0;
    auto unproj = [&](float nx, float ny, float nz, float o[3]) {
        const float* m = inv_vp;   // column-major
        const float x = m[0] * nx + m[4] * ny + m[8] * nz + m[12], y = m[1] * nx + m[5] * ny + m[9] * nz + m[13];
        const float z = m[2] * nx + m[6] * ny + m[10] * nz + m[14], w = m[3] * nx + m[7] * ny + m[11] * nz + m[15];
        const float iw = 1.f / w; o[0] = x * iw; o[1] = y * iw; o[2] = z * iw;
    };
    float c0[3], cr[3], cu[3];
    unproj(0.f, 0.f, 1.f, c0); unproj(1.f, 0.f, 1.f, cr); unproj(0.f, 1.f, 1.f, cu);
    float fwd[3] = { c0[0] - cam[0], c0[1] - cam[1], c0[2] - cam[2] };
    const float zf = std::sqrt(fwd[0] * fwd[0] + fwd[1] * fwd[1] + fwd[2] * fwd[2]);
    if (zf < 1e-3f) return 0;
    for (float& v : fwd) v /= zf;
    float rgt[3] = { cr[0] - c0[0], cr[1] - c0[1], cr[2] - c0[2] }, up[3] = { cu[0] - c0[0], cu[1] - c0[1], cu[2] - c0[2] };
    const float hw = std::sqrt(rgt[0] * rgt[0] + rgt[1] * rgt[1] + rgt[2] * rgt[2]), hh = std::sqrt(up[0] * up[0] + up[1] * up[1] + up[2] * up[2]);
    for (float& v : rgt) v /= std::max(hw, 1e-6f);
    for (float& v : up) v /= std::max(hh, 1e-6f);
    const float tanX = hw / zf * 1.15f, tanY = hh / zf * 1.15f;

    // the azimuth range of the view's footprint on the ground
    const float cosPitch = std::sqrt(fwd[0] * fwd[0] + fwd[2] * fwd[2]);
    const float yaw0 = std::atan2(fwd[0], fwd[2]);
    const float half = cosPitch < 0.25f ? 3.14159265f : std::min(3.14159265f, std::atan2(tanX, cosPitch));
    const int cols = std::max(8, std::min(128, (int)std::ceil(2.f * half / 0.05f)));

    static thread_local Want wants[kMaxWant];
    static thread_local uint16_t hash[kMaxWant * 2];   // index + 1 into wants
    std::memset(hash, 0, sizeof(hash));
    int nw = 0;
    int mip_prev = 0;
    for (int ci = 0; ci < cols; ++ci) {
        const float az = yaw0 - half + (cols > 1 ? 2.f * half * (float)ci / (float)(cols - 1) : 0.f);
        const float dx = std::sin(az), dz = std::cos(az);
        mip_prev = 0;
        for (float r = 3.f; r < 4000.f;) {
            const float x = cam[0] + dx * r, z = cam[2] + dz * r;
            if (x < fine_min[0] || z < fine_min[1] || x > fine_max[0] || z > fine_max[1]) break;
            float h = 0.f;
            float step = 0.5f * (float)kInner * TexelOf(mip_prev);
            if (hf(x, z, h)) {
                const float px = x - cam[0], py = h - cam[1], pz = z - cam[2];
                const float dist = std::sqrt(px * px + py * py + pz * pz);
                const float zc = px * fwd[0] + py * fwd[1] + pz * fwd[2];
                int mip = (int)std::floor(std::log2(std::max(dist * pix_angle / Finest(), 1e-6f)) + 0.5f);
                if (mip >= kMips) break;
                if (mip >= 0 && GradMip()) {   // the same formula as the shader: footprint of a pixel on the surface projected on XZ (Dagor tex_mip_level_sample, anisotropy limit 4)
                    const float e = 0.5f;
                    float hx0 = h, hx1 = h, hz0 = h, hz1 = h;
                    hf(x - e, z, hx0); hf(x + e, z, hx1); hf(x, z - e, hz0); hf(x, z + e, hz1);
                    float n[3] = { hx0 - hx1, 2.f * e, hz0 - hz1 };
                    const float nl = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
                    for (float& v : n) v /= nl;
                    const float vl = std::max(dist, 1e-3f);
                    const float v[3] = { px / vl, py / vl, pz / vl };
                    const float nv = std::max(std::fabs(n[0] * v[0] + n[1] * v[1] + n[2] * v[2]), 0.05f);
                    float m[3] = { v[0] - (v[0] * n[0] + v[1] * n[1] + v[2] * n[2]) * n[0], v[1] - (v[0] * n[0] + v[1] * n[1] + v[2] * n[2]) * n[1], v[2] - (v[0] * n[0] + v[1] * n[1] + v[2] * n[2]) * n[2] };
                    const float ml = std::sqrt(m[0] * m[0] + m[1] * m[1] + m[2] * m[2]);
                    if (ml > 1e-4f) {
                        for (float& c : m) c /= ml;
                        const float q[3] = { n[1] * m[2] - n[2] * m[1], n[2] * m[0] - n[0] * m[2], n[0] * m[1] - n[1] * m[0] };
                        const float s = dist * pix_angle / Finest(), L1 = s / nv, L2 = s;   // in finest texels
                        const float dx = L1 * L1 * m[0] * m[0] + L2 * L2 * q[0] * q[0], dz = L1 * L1 * m[2] * m[2] + L2 * L2 * q[2] * q[2];
                        const float lo = std::log2(std::max(std::min(dx, dz), 1e-6f)), hi = std::log2(std::max(std::max(dx, dz), 1e-6f));
                        const float mr = 0.499f * std::max(lo, hi - std::min(hi - lo, 4.f));
                        if (mr < -0.5f) mip = -1;   // finer than the finest texel: stays live (the shader's rule)
                        else {   // the level: the clamped (Dagor) form, as the shader
                            const float lc = std::log2(std::max(std::min(dx, dz), 1.f)), hc = std::log2(std::max(std::max(dx, dz), 1.f));
                            mip = std::min((int)std::floor(0.499f * std::max(lc, hc - std::min(hc - lc, 4.f)) + 0.5f), kMips - 1);
                        }
                    }
                }
                if (mip >= 0) mip_prev = mip;
                if (mip >= 0 && zc > 0.f && std::fabs(px * rgt[0] + py * rgt[1] + pz * rgt[2]) <= zc * tanX && std::fabs(px * up[0] + py * up[1] + pz * up[2]) <= zc * tanY) {
                    const float S = (float)kInner * TexelOf(mip);
                    const int ti = (int)std::floor(x / S), tj = (int)std::floor(z / S);
                    // the tile must lie inside the window with its border
                    const float bx = ti * S - kBorder * TexelOf(mip), bz = tj * S - kBorder * TexelOf(mip), ext = (float)kTexels * TexelOf(mip);
                    if (bx >= fine_min[0] && bz >= fine_min[1] && bx + ext <= fine_max[0] && bz + ext <= fine_max[1]) {
                        uint32_t hs = HashKey(mip, ti, tj) & (kMaxWant * 2 - 1);
                        bool found = false;
                        while (hash[hs]) {
                            const Want& w = wants[hash[hs] - 1];
                            if (w.mip == mip && w.ti == ti && w.tj == tj) { found = true; break; }
                            hs = (hs + 1) & (kMaxWant * 2 - 1);
                        }
                        if (!found && nw < kMaxWant) { wants[nw] = { mip, ti, tj, dist }; hash[hs] = (uint16_t)(nw + 1); ++nw; }
                    }
                }
                step = std::max(0.25f, 0.5f * (float)kInner * TexelOf(std::max(mip, 0)));
            }
            r += step;
        }
    }

    // resident tiles are refreshed; the missing ones are the candidates
    int nm = 0;
    static thread_local int missing[kMaxWant];
    for (int i = 0; i < nw; ++i) {
        const Want& w = wants[i];
        const int slot = (w.mip * kRing + (((w.tj % kRing) + kRing) % kRing)) * kRing + (((w.ti % kRing) + kRing) % kRing);
        const Entry& e = table_cpu_[slot];
        if (e.layer1 && e.ti == (uint16_t)(w.ti & 0xffff) && e.tj == (uint16_t)(w.tj & 0xffff)) layer_seen_[e.layer1 - 1] = frame;
        else missing[nm++] = i;
    }
    stats_.wanted = nw; stats_.missing = nm; stats_.layers = layers_;
    int nb = 0;
    while (nb < max_out && nm > 0) {   // nearest first
        int bi = 0;
        for (int k = 1; k < nm; ++k) if (wants[missing[k]].dist < wants[missing[bi]].dist) bi = k;
        const Want w = wants[missing[bi]];
        missing[bi] = missing[--nm];
        const int slot = (w.mip * kRing + (((w.tj % kRing) + kRing) % kRing)) * kRing + (((w.ti % kRing) + kRing) % kRing);
        Entry& e = table_cpu_[slot];
        int layer = -1;
        if (e.layer1) layer = e.layer1 - 1;   // the tile of this slot is replaced
        else if (free_top_ > 0) layer = free_[--free_top_];
        else {   // the oldest tile that was not wanted lately
            uint32_t oldest = frame - 2; int best = -1;
            for (int l = 0; l < layers_; ++l) if (layer_slot_[l] >= 0 && layer_seen_[l] < oldest) { oldest = layer_seen_[l]; best = l; }
            if (best < 0) continue;
            layer = best;
            Entry& old = table_cpu_[layer_slot_[best]];
            old.layer1 = 0; --resident_;
        }
        if (!e.layer1) ++resident_;
        e.layer1 = (uint16_t)(layer + 1); e.ti = (uint16_t)(w.ti & 0xffff); e.tj = (uint16_t)(w.tj & 0xffff);
        layer_slot_[layer] = slot; layer_seen_[layer] = frame;
        out[nb++] = { w.mip, w.ti, w.tj, layer };
        dirty_ = true;
    }
    stats_.baked_total += nb; stats_.resident = resident_;
    return nb;
}

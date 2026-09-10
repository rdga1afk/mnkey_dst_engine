#include <monkey_dust/world/transform_soa.h>
#include <monkey_dust/world/transform_soa_simd.h>
#include <monkey_dust/world/world_transform.h>
#include <monkey_dust/ecs/registry.h>
#include <monkey_dust/platform/md_log.h>
#include <monkey_dust/platform/md_hints.h>
#include <cstring>
#include <cmath>

#ifdef MD_SDL_GPU
#include <monkey_dust/render/gpu_device.h>
#include <monkey_dust/render/gpu_hal.h>
#endif

#ifdef __AVX2__
#  include <immintrin.h>   // AVX2 + FMA (Skylake / Intel HD 520)
#elif defined(__SSE2__)
#  include <xmmintrin.h>
#  include <emmintrin.h>
#endif

void TransformSoA::Init() {
    // 0xFF, not MdEntity::Null() (id_=0) -- safe under flecs only because
    // flecs::entity_t never equals UINT64_MAX for a real entity, so this
    // sentinel never collides with one (see md_entity.h's Null-convention
    // comment for the full correction, 2026-09-07).
    memset(slot_to_entity, 0xFF, sizeof(slot_to_entity));
    for (int i = 0; i < MAX_SLOTS; ++i) {
        px[i]      = DUMMY_POS;
        pz[i]      = DUMMY_POS;
        py[i]      = 0.0f;
        rot_y[i]   = 0.0f;
        dist_sq[i] = 1e18f;
        faction[i] = 0;
        // Neutral default (matches character_def.cpp's fill_default): a skin
        // tint colour with strength=0 so unset slots render with zero tint.
        skin_rgba[i][0] = 0.82f; skin_rgba[i][1] = 0.65f;
        skin_rgba[i][2] = 0.52f; skin_rgba[i][3] = 0.0f;
    }
    active_count = 0;
    transform_ring_.Init(MAX_SLOTS * 4 * sizeof(float), 0);
    faction_ssbo_.Init(MAX_SLOTS * sizeof(uint32_t));
    skin_ssbo_.Init(MAX_SLOTS * 4 * sizeof(float));

#ifdef MD_SDL_GPU
    md::GpuDeviceHandle dev = md::GpuDevice::Get().SDLDevice();
    if (dev) {
        // Init() runs twice per process: once at startup (game_init.cpp) and
        // once per `load game` (world_serializer.cpp's Deserialize, which
        // resets the world without a fresh TransformSoA instance). Release
        // any buffers from a prior Init() first -- otherwise every load
        // overwrites the handles and leaks the old ones (DEFECT_INVENTORY.md
        // #4). SDL_GPU defers the actual GPU-side free until no in-flight
        // command buffer references the resource, so releasing here is safe
        // even if the previous frame's draw is still executing.
        for (int s = 0; s < 3; ++s) {
            if (sdl_xzyr_buf_[s])    { SDL_ReleaseGPUBuffer(dev, sdl_xzyr_buf_[s]);       sdl_xzyr_buf_[s]    = nullptr; }
            if (sdl_xzyr_stg_[s])    { GpuReleaseTransferBuffer(dev, sdl_xzyr_stg_[s]);    sdl_xzyr_stg_[s]    = nullptr; }
            if (sdl_faction_buf_[s]) { SDL_ReleaseGPUBuffer(dev, sdl_faction_buf_[s]);     sdl_faction_buf_[s] = nullptr; }
            if (sdl_faction_stg_[s]) { GpuReleaseTransferBuffer(dev, sdl_faction_stg_[s]); sdl_faction_stg_[s] = nullptr; }
            if (sdl_skin_buf_[s])    { SDL_ReleaseGPUBuffer(dev, sdl_skin_buf_[s]);        sdl_skin_buf_[s]    = nullptr; }
            if (sdl_skin_stg_[s])    { GpuReleaseTransferBuffer(dev, sdl_skin_stg_[s]);    sdl_skin_stg_[s]    = nullptr; }
        }

        static constexpr uint32_t XZYR_SIZE = MAX_SLOTS * 4 * sizeof(float);
        static constexpr uint32_t FACI_SIZE = MAX_SLOTS * sizeof(uint32_t);
        static constexpr uint32_t SKIN_SIZE = MAX_SLOTS * 4 * sizeof(float);

        SDL_GPUBufferCreateInfo bi = {};
        bi.usage = SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ |
                   SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ;

        SDL_GPUTransferBufferCreateInfo ti = {};
        ti.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;

        for (int s = 0; s < 3; ++s) {
            bi.size = XZYR_SIZE;
            sdl_xzyr_buf_[s] = SDL_CreateGPUBuffer(dev, &bi);
            if (!sdl_xzyr_buf_[s])
                MD_LOG(MD_LOG_WARNING, "[TransformSoA] SDL xzyr buf[%d] failed: %s", s, SDL_GetError());

            bi.size = FACI_SIZE;
            sdl_faction_buf_[s] = SDL_CreateGPUBuffer(dev, &bi);
            if (!sdl_faction_buf_[s])
                MD_LOG(MD_LOG_WARNING, "[TransformSoA] SDL faction buf[%d] failed: %s", s, SDL_GetError());

            ti.size = XZYR_SIZE;
            sdl_xzyr_stg_[s] = GpuCreateTransferBuffer(dev, &ti);

            ti.size = FACI_SIZE;
            sdl_faction_stg_[s] = GpuCreateTransferBuffer(dev, &ti);

            bi.size = SKIN_SIZE;
            sdl_skin_buf_[s] = SDL_CreateGPUBuffer(dev, &bi);
            if (!sdl_skin_buf_[s])
                MD_LOG(MD_LOG_WARNING, "[TransformSoA] SDL skin buf[%d] failed: %s", s, SDL_GetError());

            ti.size = SKIN_SIZE;
            sdl_skin_stg_[s] = GpuCreateTransferBuffer(dev, &ti);
        }
    }
#endif
}

uint32_t TransformSoA::Alloc(MdEntity e, float x, float z, uint8_t faction_id) {
    if (active_count >= MAX_SLOTS) {
        MD_LOG(MD_LOG_WARNING, "[TransformSoA] Alloc: out of slots (%d)", MAX_SLOTS);
        return INVALID_SLOT;
    }
    uint32_t slot = (uint32_t)active_count++;
    px[slot]      = x;
    pz[slot]      = z;
    py[slot]      = 0.0f;
    rot_y[slot]   = 0.0f;
    dist_sq[slot] = 1e18f;
    faction[slot] = faction_id;
    slot_to_entity[slot] = e;
    MarkFactionDirty(slot);
    MarkTransformDirty(slot);
    return slot;
}

void TransformSoA::Free(MdEntity e) {
    auto& reg = MdRegistry::Get();
    if (!reg.Valid(e) || !(reg.Handle(e).has<WorldTransform>())) return;
    auto& tr  = reg.Handle(e).get_mut<WorldTransform>();
    uint32_t slot = tr.slot;
    if (slot == INVALID_SLOT || slot >= (uint32_t)active_count) return;
    tr.slot = INVALID_SLOT;

    uint32_t last = (uint32_t)(--active_count);
    if (slot != last) {
        px[slot]      = px[last];
        pz[slot]      = pz[last];
        py[slot]      = py[last];
        rot_y[slot]   = rot_y[last];
        dist_sq[slot] = dist_sq[last];
        faction[slot] = faction[last];
        MarkFactionDirty(slot);
        skin_rgba[slot][0] = skin_rgba[last][0];
        skin_rgba[slot][1] = skin_rgba[last][1];
        skin_rgba[slot][2] = skin_rgba[last][2];
        skin_rgba[slot][3] = skin_rgba[last][3];
        MarkSkinDirty(slot);
        MdEntity moved = slot_to_entity[last];
        slot_to_entity[slot] = moved;
        if (reg.Valid(moved) && (reg.Handle(moved).has<WorldTransform>()))
            reg.Handle(moved).get_mut<WorldTransform>().slot = slot;
    }
    px[last]      = DUMMY_POS;
    pz[last]      = DUMMY_POS;
    slot_to_entity[last] = MdEntity::Null();
}

void TransformSoA::FlushAoStoSoA(MdRegistry& reg) {
    static auto q_p3_transform_soa_1 = reg.Raw().query().all<WorldTransform>();
    MdEach(q_p3_transform_soa_1, [](const WorldTransform& tr) {
        if (MD_UNLIKELY(tr.slot == INVALID_SLOT ||
                        tr.slot >= (uint32_t)TransformSoA::Get().active_count))
            return;
        auto& s = TransformSoA::Get();
        // Skip SoA write + dirty mark if position unchanged (static NPCs).
        if (s.px[tr.slot]    == tr.x   && s.pz[tr.slot] == tr.z &&
            s.py[tr.slot]    == tr.y   && s.rot_y[tr.slot] == tr.rot_y)
            return;
        s.px[tr.slot]    = tr.x;
        s.pz[tr.slot]    = tr.z;
        s.py[tr.slot]    = tr.y;
        s.rot_y[tr.slot] = tr.rot_y;
        s.MarkTransformDirty(tr.slot);
    });
}

void TransformSoA::UploadToGPU() {
    if (active_count <= 0) return;

    // Write transform data directly into the ring buffer's current slot (zero-copy).
    const uint32_t xzyr_bytes = (uint32_t)active_count * 4 * sizeof(float);
    float* dst = static_cast<float*>(transform_ring_.MapWrite());
    if (dst) {
        for (int i = 0; i < active_count; ++i) {
            dst[i*4+0] = px[i];
            dst[i*4+1] = pz[i];
            dst[i*4+2] = py[i];
            dst[i*4+3] = rot_y[i];
        }
        transform_ring_.Unmap();
    }
    transform_ring_.BindStorage(0);

    // Faction data rarely changes → upload only the dirty slot range.
    if (faction_dirty_ && active_count > 0) {
        uint32_t lo = faction_dirty_min_;
        uint32_t hi = faction_dirty_max_;
        if (hi >= (uint32_t)active_count) hi = (uint32_t)active_count - 1;
        if (lo > hi) lo = hi;
        uint32_t cnt = hi - lo + 1;
        static uint32_t ff[MAX_SLOTS];
        for (uint32_t i = lo; i <= hi; ++i) ff[i - lo] = (uint32_t)faction[i];
        faction_ssbo_.Upload(ff, (int)(cnt * sizeof(uint32_t)),
                             (int)(lo * sizeof(uint32_t)));
        faction_dirty_ = false;
    }
    faction_ssbo_.Bind(3);

    // Skin tint data rarely changes → upload only the dirty slot range.
    if (skin_dirty_ && active_count > 0) {
        uint32_t lo = skin_dirty_min_;
        uint32_t hi = skin_dirty_max_;
        if (hi >= (uint32_t)active_count) hi = (uint32_t)active_count - 1;
        if (lo > hi) lo = hi;
        uint32_t cnt = hi - lo + 1;
        skin_ssbo_.Upload(&skin_rgba[lo][0], (int)(cnt * 4 * sizeof(float)),
                          (int)(lo * 4 * sizeof(float)));
        skin_dirty_ = false;
    }
    skin_ssbo_.Bind(4);
}

void TransformSoA::AdvanceFrame() {
    transform_ring_.Advance();
}

MD_HOT void TransformSoA::BulkComputeDistSq(float cam_x, float cam_z) {
#ifdef __AVX2__
    // Extracted to transform_soa_simd_avx2.cpp (Phase 2.2/3 audit) — same
    // math as before, now independently testable/callable regardless of
    // this TU's own compile flags. Behavior unchanged.
    md::simd::ComputeDistSqAVX2(px, pz, cam_x, cam_z, dist_sq, active_count);
#elif defined(__SSE2__)
    __m128 cx4 = _mm_set1_ps(cam_x);
    __m128 cz4 = _mm_set1_ps(cam_z);
    int n4 = active_count & ~3;
    for (int i = 0; i < n4; i += 4) {
        __m128 dx = _mm_sub_ps(_mm_load_ps(px + i), cx4);
        __m128 dz = _mm_sub_ps(_mm_load_ps(pz + i), cz4);
        _mm_store_ps(dist_sq + i, _mm_add_ps(_mm_mul_ps(dx, dx), _mm_mul_ps(dz, dz)));
    }
    for (int i = n4; i < active_count; ++i) {
        float dx = px[i] - cam_x, dz = pz[i] - cam_z;
        dist_sq[i] = dx*dx + dz*dz;
    }
#else
    md::simd::ComputeDistSqScalar(px, pz, cam_x, cam_z, dist_sq, active_count);
#endif
}

MD_HOT void TransformSoA::BulkComputeLOD(float near_sq, float far_sq, uint8_t* out_lod) const {
#ifdef __AVX2__
    __m256 near8 = _mm256_set1_ps(near_sq);
    __m256 far8  = _mm256_set1_ps(far_sq);
    int n8 = active_count & ~7;
    for (int i = 0; i < n8; i += 8) {
        __m256 d   = _mm256_load_ps(dist_sq + i);
        int mask_n = _mm256_movemask_ps(_mm256_cmp_ps(d, near8, _CMP_LT_OQ));
        int mask_f = _mm256_movemask_ps(_mm256_cmp_ps(d, far8,  _CMP_LT_OQ));
        for (int j = 0; j < 8; ++j)
            out_lod[i+j] = (mask_n >> j) & 1 ? 0 : ((mask_f >> j) & 1 ? 1 : 2);
    }
    for (int i = n8; i < active_count; ++i)
        out_lod[i] = dist_sq[i] < near_sq ? 0 : (dist_sq[i] < far_sq ? 1 : 2);
#elif defined(__SSE2__)
    __m128 near4 = _mm_set1_ps(near_sq);
    __m128 far4  = _mm_set1_ps(far_sq);
    int n4 = active_count & ~3;
    for (int i = 0; i < n4; i += 4) {
        __m128 d      = _mm_load_ps(dist_sq + i);
        int mask_n    = _mm_movemask_ps(_mm_cmplt_ps(d, near4));
        int mask_f    = _mm_movemask_ps(_mm_cmplt_ps(d, far4));
        for (int j = 0; j < 4; ++j)
            out_lod[i+j] = (mask_n >> j) & 1 ? 0 : ((mask_f >> j) & 1 ? 1 : 2);
    }
    for (int i = n4; i < active_count; ++i)
        out_lod[i] = dist_sq[i] < near_sq ? 0 : (dist_sq[i] < far_sq ? 1 : 2);
#else
    for (int i = 0; i < active_count; ++i)
        out_lod[i] = dist_sq[i] < near_sq ? 0 : (dist_sq[i] < far_sq ? 1 : 2);
#endif
}

int TransformSoA::BuildMatrices(const uint8_t* lod, float far_sq,
                                 Mat4* out_matrices, int max_out) const {
    int count = 0;
    for (int i = 0; i < active_count && count < max_out; ++i) {
        if (lod && lod[i] >= 2) continue;
        if (px[i] >= DUMMY_POS) continue;
        out_matrices[count++] = BuildSingleMatrix((uint32_t)i);
    }
    return count;
}

Mat4 TransformSoA::BuildSingleMatrix(uint32_t slot) const {
    return mat4_mul(mat4_rotate_y(rot_y[slot]),
                    mat4_translate(px[slot], py[slot] + 0.9f, pz[slot]));
}

// Save v5 accessors (БОРГ-6)
uint32_t TransformSoA::GetSlotForEntity(MdEntity e) const {
    for (int i = 0; i < active_count; ++i)
        if (slot_to_entity[i] == e) return (uint32_t)i;
    return INVALID_SLOT;
}

void TransformSoA::AssignSlot(MdEntity e, uint32_t slot,
                               float x, float z, uint8_t faction_id) {
    if (slot >= MAX_SLOTS) {
        MD_LOG(MD_LOG_WARNING, "[TransformSoA] AssignSlot: slot %u out of range", slot);
        return;
    }
    px[slot]    = x;
    pz[slot]    = z;
    py[slot]    = 0.0f;
    rot_y[slot] = 0.0f;
    dist_sq[slot] = 1e18f;
    faction[slot] = faction_id;
    MarkFactionDirty(slot);
    slot_to_entity[slot] = e;
    if ((int)slot >= active_count) active_count = (int)slot + 1;
}

#ifdef MD_SDL_GPU
SDL_GPUBuffer* TransformSoA::SDLTransformBuffer() const {
    return sdl_xzyr_buf_[md::GpuDevice::Get().FrameSlot()];
}
SDL_GPUBuffer* TransformSoA::SDLFactionBuffer() const {
    return sdl_faction_buf_[md::GpuDevice::Get().FrameSlot()];
}
SDL_GPUBuffer* TransformSoA::SDLSkinBuffer() const {
    return sdl_skin_buf_[md::GpuDevice::Get().FrameSlot()];
}

void TransformSoA::UploadSDLGPU(md::GpuCommandBufferHandle cmd) {
    if (!cmd || active_count <= 0) return;
    int s = md::GpuDevice::Get().FrameSlot();

    // Upload all active xzyr every frame (full upload to current slot — no dirty-range
    // since triple-buffering means cycle=false is safe: GPU never reads this slot).
    if (sdl_xzyr_stg_[s] && sdl_xzyr_buf_[s]) {
        uint32_t byte_sz = (uint32_t)active_count * 4 * sizeof(float);
        float* dst = (float*)GpuMapTransfer(sdl_xzyr_stg_[s], false);
        if (dst) {
            for (int i = 0; i < active_count; ++i) {
                dst[i*4+0] = px[i];
                dst[i*4+1] = pz[i];
                dst[i*4+2] = py[i];
                dst[i*4+3] = rot_y[i];
            }
            GpuUnmapTransfer(sdl_xzyr_stg_[s]);
        }
        GpuCopyPass cpass;
        cpass.Begin(cmd);
        SDL_GPUTransferBufferLocation src = {};
        src.transfer_buffer = sdl_xzyr_stg_[s];
        SDL_GPUBufferRegion dst_r = {};
        dst_r.buffer = sdl_xzyr_buf_[s];
        dst_r.size   = byte_sz;
        cpass.UploadBuffer(src, dst_r, false);
        cpass.End();
        xzyr_dirty_ = false;
    }

    // Upload faction data to current slot.
    if (sdl_faction_stg_[s] && sdl_faction_buf_[s]) {
        uint32_t* dst = (uint32_t*)GpuMapTransfer(sdl_faction_stg_[s], false);
        if (dst) {
            for (int i = 0; i < active_count; ++i)
                dst[i] = (uint32_t)faction[i];
            GpuUnmapTransfer(sdl_faction_stg_[s]);
        }
        GpuCopyPass cpass;
        cpass.Begin(cmd);
        SDL_GPUTransferBufferLocation src = {};
        src.transfer_buffer = sdl_faction_stg_[s];
        SDL_GPUBufferRegion dst_r = {};
        dst_r.buffer = sdl_faction_buf_[s];
        dst_r.size   = (Uint32)(active_count * sizeof(uint32_t));
        cpass.UploadBuffer(src, dst_r, false);
        cpass.End();
    }

    // Upload skin tint data to current slot (rarely changes; re-uploaded
    // every frame like faction — cost scales with active_count, not MAX_SLOTS).
    if (sdl_skin_stg_[s] && sdl_skin_buf_[s]) {
        float* dst = (float*)GpuMapTransfer(sdl_skin_stg_[s], false);
        if (dst) {
            memcpy(dst, skin_rgba, (size_t)active_count * 4 * sizeof(float));
            GpuUnmapTransfer(sdl_skin_stg_[s]);
        }
        GpuCopyPass cpass;
        cpass.Begin(cmd);
        SDL_GPUTransferBufferLocation src = {};
        src.transfer_buffer = sdl_skin_stg_[s];
        SDL_GPUBufferRegion dst_r = {};
        dst_r.buffer = sdl_skin_buf_[s];
        dst_r.size   = (Uint32)(active_count * 4 * sizeof(float));
        cpass.UploadBuffer(src, dst_r, false);
        cpass.End();
    }
}
#endif

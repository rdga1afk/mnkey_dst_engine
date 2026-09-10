#include <monkey_dust/render/gpu_hal.h>
#include <monkey_dust/render/gpu_resource_tracker.h>
#include <monkey_dust/platform/md_log.h>
#include <cstdlib>
#include <cstdio>
#include <cstring>

#include "glad.h"

#ifdef MD_SDL_GPU
#include <monkey_dust/render/gpu_device.h>
#endif

// ── Common helpers ─────────────────────────────────────────────────────────────

static char* ReadTextFile(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) return nullptr;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    rewind(f);
    char* buf = (char*)malloc((size_t)len + 1);
    if (!buf) { fclose(f); return nullptr; }
    fread(buf, 1, (size_t)len, f);
    buf[len] = '\0';
    fclose(f);
    return buf;
}

#ifdef MD_SDL_GPU

// ── SDL_GPUGraphicsPipeline object cache ─────────────────────────────────────
// Vulkan pattern: VkPipelineCache reuses driver-compiled binaries.
// SDL_GPU adaptation: cache compiled SDL_GPUGraphicsPipeline objects by
// (vert_path, frag_path, shader_features, raster_hash) key. Hot-reload and
// multiple systems requesting the same pipeline config avoid redundant
// SDL_CreateGPUGraphicsPipeline calls (each compiles SPIR-V → driver binary).

static constexpr int PIPE_CACHE_MAX = 64;

struct PipeEntry {
    uint32_t              hash     = 0;
    SDL_GPUGraphicsPipeline* pipe  = nullptr;
    bool                  used     = false;
};

static PipeEntry s_pipe_cache[PIPE_CACHE_MAX];

static uint32_t PipeHash(const char* vert, const char* frag,
                          uint32_t features, uint32_t raster_bits) {
    uint32_t h = 2166136261u;
    auto mix = [&](const char* s) {
        for (; s && *s; ++s) h = (h ^ (uint8_t)*s) * 16777619u;
        h = (h ^ 0xFF) * 16777619u; // separator
    };
    mix(vert); mix(frag);
    h = (h ^ features)    * 16777619u;
    h = (h ^ raster_bits) * 16777619u;
    return h ? h : 1u;
}

static SDL_GPUGraphicsPipeline* PipeCache_Get(uint32_t hash) {
    for (int i = 0; i < PIPE_CACHE_MAX; ++i)
        if (s_pipe_cache[i].used && s_pipe_cache[i].hash == hash)
            return s_pipe_cache[i].pipe;
    return nullptr;
}

static void PipeCache_Put(uint32_t hash, SDL_GPUGraphicsPipeline* pipe) {
    for (int i = 0; i < PIPE_CACHE_MAX; ++i) {
        if (!s_pipe_cache[i].used) {
            s_pipe_cache[i] = { hash, pipe, true };
            return;
        }
    }
    // Full: evict slot 0 (oldest, simple FIFO for this small table)
    if (s_pipe_cache[0].pipe)
        SDL_ReleaseGPUGraphicsPipeline(md::GpuDevice::Get().SDLDevice(),
                                        s_pipe_cache[0].pipe);
    // Shift left and insert at end
    for (int i = 0; i < PIPE_CACHE_MAX - 1; ++i) s_pipe_cache[i] = s_pipe_cache[i+1];
    s_pipe_cache[PIPE_CACHE_MAX-1] = { hash, pipe, true };
}

void MdPipeCache_Invalidate(const char* vert_path, const char* frag_path) {
    // Called by hot-reload to evict stale pipelines for a given shader pair.
    uint32_t h_base = PipeHash(vert_path, frag_path, 0, 0) & 0xFFFF0000u;
    SDL_GPUDevice* dev = md::GpuDevice::Get().SDLDevice();
    for (int i = 0; i < PIPE_CACHE_MAX; ++i) {
        if (s_pipe_cache[i].used && (s_pipe_cache[i].hash & 0xFFFF0000u) == h_base) {
            if (dev && s_pipe_cache[i].pipe)
                SDL_ReleaseGPUGraphicsPipeline(dev, s_pipe_cache[i].pipe);
            s_pipe_cache[i] = {};
        }
    }
}

void MdPipeCache_Shutdown() {
    SDL_GPUDevice* dev = md::GpuDevice::Get().SDLDevice();
    int freed = 0;
    for (int i = 0; i < PIPE_CACHE_MAX; ++i) {
        if (s_pipe_cache[i].used) {
            if (dev && s_pipe_cache[i].pipe)
                SDL_ReleaseGPUGraphicsPipeline(dev, s_pipe_cache[i].pipe);
            s_pipe_cache[i] = {};
            ++freed;
        }
    }
    if (freed) fprintf(stdout, "[PipeCache] shutdown: released %d pipelines\n", freed);
}

// ── SPIR-V bytecode cache ─────────────────────────────────────────────────────
// Port of VBfA pattern: binary shader blobs cached in memory to avoid repeated
// disk reads (hot-reload path, shared vert shaders across pipeline variants).
//
// Capacity 128 (up from 64) — handles current ~60 .spv files with headroom.
// Eviction: LRU via generation counter when full (no silent leak on overflow).

static constexpr int SPV_CACHE_MAX = 128;

struct SpvEntry {
    uint32_t hash    = 0;
    void*    data    = nullptr;
    size_t   size    = 0;
    uint32_t gen     = 0;   // LRU generation (incremented on each use)
    bool     used    = false;
};

static SpvEntry  s_spv_cache[SPV_CACHE_MAX];
static uint32_t  s_spv_gen = 0;

static uint32_t SpvHash(const char* s) {
    uint32_t h = 2166136261u;
    for (; *s; ++s) h = (h ^ (uint8_t)*s) * 16777619u;
    return h ? h : 1u;
}

static void* ReadBinaryFile(const char* path, size_t* out_size) {
    uint32_t h = SpvHash(path);
    // Cache hit — update LRU generation, return cached pointer.
    for (int i = 0; i < SPV_CACHE_MAX; ++i) {
        if (s_spv_cache[i].used && s_spv_cache[i].hash == h) {
            s_spv_cache[i].gen = ++s_spv_gen;
            *out_size = s_spv_cache[i].size;
            return s_spv_cache[i].data;
        }
    }
    // Cache miss: read from disk.
    FILE* f = fopen(path, "rb");
    if (!f) return nullptr;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    rewind(f);
    void* buf = malloc((size_t)len);
    if (!buf) { fclose(f); return nullptr; }
    fread(buf, 1, (size_t)len, f);
    fclose(f);
    *out_size = (size_t)len;
    // Find empty slot or evict LRU.
    int slot = -1;
    for (int i = 0; i < SPV_CACHE_MAX; ++i) {
        if (!s_spv_cache[i].used) { slot = i; break; }
    }
    if (slot < 0) {
        // All slots used — evict least-recently-used entry.
        uint32_t oldest_gen = s_spv_cache[0].gen;
        slot = 0;
        for (int i = 1; i < SPV_CACHE_MAX; ++i) {
            if (s_spv_cache[i].gen < oldest_gen) {
                oldest_gen = s_spv_cache[i].gen;
                slot = i;
            }
        }
        free(s_spv_cache[slot].data);
    }
    s_spv_cache[slot] = { h, buf, (size_t)len, ++s_spv_gen, true };
    return buf;
}

// Returns true if ptr is owned by the cache (must not be freed by caller).
static bool SpvIsCached(const void* ptr) {
    for (int i = 0; i < SPV_CACHE_MAX; ++i)
        if (s_spv_cache[i].used && s_spv_cache[i].data == ptr) return true;
    return false;
}

void MdSpvCache_Shutdown() {
    int freed = 0;
    for (int i = 0; i < SPV_CACHE_MAX; ++i) {
        if (s_spv_cache[i].used) {
            free(s_spv_cache[i].data);
            s_spv_cache[i] = {};
            ++freed;
        }
    }
    s_spv_gen = 0;
    if (freed) fprintf(stdout, "[SpvCache] shutdown: freed %d SPIR-V entries\n", freed);
}

int MdSpvCache_Stats(int* out_count) {
    int n = 0;
    for (int i = 0; i < SPV_CACHE_MAX; ++i)
        if (s_spv_cache[i].used) ++n;
    if (out_count) *out_count = n;
    return n;
}


// Derive SPIR-V path with feature suffix (specialization constant variant selection).
// "shaders/pbr.frag" + SF_PCF_HIGH → "shaders/spirv/pbr.frag_pcfhigh.spv"
// Falls back to base variant if feature variant doesn't exist on disk.
// This is the SDL_GPU adaptation of VkSpecializationInfo: pre-compiled per-variant SPIR-V.
static void MakeSpvPath(char* out, size_t out_sz, const char* glsl_path,
                         uint32_t features = 0) {
    const char* slash = strrchr(glsl_path, '/');
    const char* name  = slash ? slash + 1 : glsl_path;

    // Build feature suffix from specialization bits.
    char suffix[64] = {};
    int  slen       = 0;
    auto app = [&](const char* s) {
        while (*s && slen < (int)sizeof(suffix) - 1) suffix[slen++] = *s++;
    };
    if (features & SF_PCF_HIGH)  app("_pcfhigh");
    if (features & SF_SSAO_INT)  app("_ssao");
    if (features & SF_IBL)       app("_ibl");
    if (features & SF_DITHERED)  app("_dither");

    if (slen > 0) {
        // Try variant first, fall back to base.
        char variant[256];
        snprintf(variant, sizeof(variant), "shaders/spirv/%s%s.spv", name, suffix);
        FILE* f = fopen(variant, "rb");
        if (f) { fclose(f); snprintf(out, out_sz, "%s", variant); return; }
    }
    snprintf(out, out_sz, "shaders/spirv/%s.spv", name);
}

void MdSpvCache_Invalidate(const char* glsl_path) {
    char spv[256]; MakeSpvPath(spv, sizeof(spv), glsl_path);
    uint32_t h = SpvHash(spv);
    for (int i = 0; i < SPV_CACHE_MAX; ++i) {
        if (s_spv_cache[i].used && s_spv_cache[i].hash == h) {
            free(s_spv_cache[i].data);
            s_spv_cache[i] = {};
            return;
        }
    }
}

static SDL_GPUVertexElementFormat ToSDLFmt(GpuAttribFmt f) {
    switch (f) {
    case GpuAttribFmt::F1:        return SDL_GPU_VERTEXELEMENTFORMAT_FLOAT;
    case GpuAttribFmt::F2:        return SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2;
    case GpuAttribFmt::F3:        return SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3;
    case GpuAttribFmt::F4:        return SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4;
    case GpuAttribFmt::U8x4_NORM: return SDL_GPU_VERTEXELEMENTFORMAT_UBYTE4_NORM;
    case GpuAttribFmt::U8x4:      return SDL_GPU_VERTEXELEMENTFORMAT_UBYTE4;
    }
    return SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3;
}

static SDL_GPUPrimitiveType ToSDLPrim(GpuTopology t) {
    switch (t) {
    case GpuTopology::POINTS: return SDL_GPU_PRIMITIVETYPE_POINTLIST;
    case GpuTopology::LINES:  return SDL_GPU_PRIMITIVETYPE_LINELIST;
    default:                  return SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
    }
}

static SDL_GPUBlendFactor ToSDLBlend(GpuBlendFactor f) {
    switch (f) {
    case GpuBlendFactor::ZERO:                return SDL_GPU_BLENDFACTOR_ZERO;
    case GpuBlendFactor::ONE:                 return SDL_GPU_BLENDFACTOR_ONE;
    case GpuBlendFactor::SRC_COLOR:           return SDL_GPU_BLENDFACTOR_SRC_COLOR;
    case GpuBlendFactor::ONE_MINUS_SRC_COLOR: return SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_COLOR;
    case GpuBlendFactor::SRC_ALPHA:           return SDL_GPU_BLENDFACTOR_SRC_ALPHA;
    case GpuBlendFactor::ONE_MINUS_SRC_ALPHA: return SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
    case GpuBlendFactor::DST_ALPHA:           return SDL_GPU_BLENDFACTOR_DST_ALPHA;
    case GpuBlendFactor::ONE_MINUS_DST_ALPHA: return SDL_GPU_BLENDFACTOR_ONE_MINUS_DST_ALPHA;
    case GpuBlendFactor::DST_COLOR:           return SDL_GPU_BLENDFACTOR_DST_COLOR;
    case GpuBlendFactor::ONE_MINUS_DST_COLOR: return SDL_GPU_BLENDFACTOR_ONE_MINUS_DST_COLOR;
    }
    return SDL_GPU_BLENDFACTOR_ONE;
}

static SDL_GPUShader* LoadSpvShader(SDL_GPUDevice* dev,
                                    const char*    spv_path,
                                    SDL_GPUShaderStage stage,
                                    uint32_t num_uniform_bufs,
                                    uint32_t num_storage_bufs,
                                    uint32_t num_samplers) {
    size_t code_size = 0;
    void*  code      = ReadBinaryFile(spv_path, &code_size);
    if (!code) {
        MD_LOG(MD_LOG_WARNING, "[GpuPipeline] SPIR-V not found: %s", spv_path);
        return nullptr;
    }
    SDL_GPUShaderCreateInfo info = {};
    info.code              = (const Uint8*)code;
    info.code_size         = code_size;
    info.entrypoint        = "main";
    info.format            = SDL_GPU_SHADERFORMAT_SPIRV;
    info.stage             = stage;
    info.num_uniform_buffers = num_uniform_bufs;
    info.num_storage_buffers = num_storage_bufs;
    info.num_samplers      = num_samplers;
    SDL_GPUShader* sh = SDL_CreateGPUShader(dev, &info);
    // Only free if not owned by the SPIR-V cache.
    if (!SpvIsCached(code)) free(code);
    if (!sh) MD_LOG(MD_LOG_WARNING, "[GpuPipeline] SDL_CreateGPUShader failed: %s", SDL_GetError());
    return sh;
}

#endif // MD_SDL_GPU

// ── GpuPipeline ───────────────────────────────────────────────────────────────

bool GpuPipeline::Create(const Desc& desc) {
    if (!desc.vert_path || !desc.frag_path) {
        MD_LOG(MD_LOG_WARNING, "[GpuPipeline] Create: null shader paths");
        return false;
    }
    desc_   = desc;
    raster_ = desc.raster;

#ifdef MD_SDL_GPU
    // ── Intel HD 520 / ANV binding validation ─────────────────────────────────
    // These combinations are silent GPU hangs or garbage output on Gen9 ANV.
    // Fail fast at pipeline creation rather than debugging corrupted frames.
#ifdef DEBUG
    // 2026-07-25: vert_samplers>0 + frag_samplers>0 downgraded from a hard
    // block to a log-only note. Was documented as a full-OS-freeze hang
    // (~2026-06-01) — empirically DISPROVEN via an isolated standalone test
    // (real vertex texture fetch displacing position + a fragment sampler,
    // full draw call, Vulkan validation layer active): 0 hang, 0 crash, 0
    // validation errors on this exact Intel HD 520/Mesa 26.1.4/SDL3 3.4.12
    // stack. Driver/SDL3 updates since June most likely fixed the original
    // bug. Kept as a log line (not a hard return false) since the test was
    // a minimal isolated case, not this project's full shader complexity —
    // if something unexpected shows up on a real vertex-texture-fetch
    // shader, this line is where to look first, but it should no longer
    // block legitimate work outright.
    if (desc.vert_samplers > 0 && desc.frag_samplers > 0) {
        MD_LOG(MD_LOG_INFO,
            "[GpuPipeline] vert_samplers=%u + frag_samplers=%u — previously "
            "documented as a Gen9 ANV hang, disproven 2026-07-25 (isolated "
            "test, current Mesa/SDL3). Proceeding. (%s)",
            desc.vert_samplers, desc.frag_samplers, desc.vert_path);
    }
    if (desc.vert_storage_bufs > 0 && desc.frag_samplers > 0) {
        MD_LOG(MD_LOG_WARNING,
            "[GpuPipeline] INVALID: vert_storage_bufs=%u + frag_samplers=%u — "
            "silent fail on SDL3 3.4.8 (binding N+1 garbage). "
            "Use vert SSBO via location=4 workaround. (%s)",
            desc.vert_storage_bufs, desc.frag_samplers, desc.vert_path);
        return false;
    }
    if (desc.has_depth_target && !desc.depth_only && desc.frag_samplers == 0
        && desc.frag_storage_bufs == 0 && desc.frag_uniform_bufs == 0) {
        MD_LOG(MD_LOG_WARNING,
            "[GpuPipeline] SUSPICIOUS: depth enabled but zero frag resources — "
            "check frag_uniform_bufs matches SPIR-V set=3 binding count. (%s / %s)",
            desc.vert_path, desc.frag_path);
    }
#endif // DEBUG

    SDL_GPUDevice* dev = md::GpuDevice::Get().SDLDevice();
    if (!dev) {
        MD_LOG(MD_LOG_WARNING, "[GpuPipeline] SDL_GPU not ready");
        return false;
    }

    char spv_vert[256], spv_frag[256];
    MakeSpvPath(spv_vert, sizeof(spv_vert), desc.vert_path, desc.shader_features);
    MakeSpvPath(spv_frag, sizeof(spv_frag), desc.frag_path, desc.shader_features);

    SDL_GPUShader* vert_sh = LoadSpvShader(dev, spv_vert,
        SDL_GPU_SHADERSTAGE_VERTEX,
        desc.vert_uniform_bufs, desc.vert_storage_bufs, desc.vert_samplers);
    SDL_GPUShader* frag_sh = LoadSpvShader(dev, spv_frag,
        SDL_GPU_SHADERSTAGE_FRAGMENT,
        desc.frag_uniform_bufs, desc.frag_storage_bufs, desc.frag_samplers);

    if (!vert_sh || !frag_sh) {
        if (vert_sh) SDL_ReleaseGPUShader(dev, vert_sh);
        if (frag_sh) SDL_ReleaseGPUShader(dev, frag_sh);
        return false;
    }

    // Vertex input state
    SDL_GPUVertexBufferDescription vbds[2] = {};
    vbds[0].slot               = 0;
    vbds[0].pitch              = desc.layout.stride;
    vbds[0].input_rate         = SDL_GPU_VERTEXINPUTRATE_VERTEX;
    vbds[0].instance_step_rate = 0;
    const bool has_inst = (desc.layout.inst_stride > 0 && desc.layout.inst_count > 0);
    if (has_inst) {
        vbds[1].slot               = 1;
        vbds[1].pitch              = desc.layout.inst_stride;
        vbds[1].input_rate         = desc.layout.inst_per_vertex
                                     ? SDL_GPU_VERTEXINPUTRATE_VERTEX
                                     : SDL_GPU_VERTEXINPUTRATE_INSTANCE;
        vbds[1].instance_step_rate = 0;
    }

    SDL_GPUVertexAttribute vattribs[16] = {};
    uint32_t total_attribs = 0;
    for (uint32_t i = 0; i < desc.layout.count; ++i, ++total_attribs) {
        vattribs[total_attribs].location    = desc.layout.attribs[i].location;
        vattribs[total_attribs].buffer_slot = 0;
        vattribs[total_attribs].format      = ToSDLFmt(desc.layout.attribs[i].fmt);
        vattribs[total_attribs].offset      = desc.layout.attribs[i].offset;
    }
    if (has_inst) {
        for (uint32_t i = 0; i < desc.layout.inst_count; ++i, ++total_attribs) {
            vattribs[total_attribs].location    = desc.layout.inst_attribs[i].location;
            vattribs[total_attribs].buffer_slot = 1;
            vattribs[total_attribs].format      = ToSDLFmt(desc.layout.inst_attribs[i].fmt);
            vattribs[total_attribs].offset      = desc.layout.inst_attribs[i].offset;
        }
    }

    SDL_GPUVertexInputState vertex_input = {};
    vertex_input.vertex_buffer_descriptions = (desc.layout.stride > 0) ? vbds : nullptr;
    vertex_input.num_vertex_buffers         = (desc.layout.stride > 0) ? (has_inst ? 2u : 1u) : 0u;
    vertex_input.vertex_attributes          = (total_attribs > 0) ? vattribs : nullptr;
    vertex_input.num_vertex_attributes      = total_attribs;

    // Color target (uses swapchain format); skipped for depth_only passes.
    SDL_GPUColorTargetDescription color_target = {};
    if (!desc.depth_only) {
        SDL_GPUTextureFormat fmt = desc.color_format;
        if (fmt == SDL_GPU_TEXTUREFORMAT_INVALID) {
            SDL_Window* win = md::GpuDevice::Get().Window();
            fmt = SDL_GetGPUSwapchainTextureFormat(dev, win);
        }
        color_target.format = fmt;
        if (desc.raster.blend_enable) {
            color_target.blend_state.enable_blend          = true;
            color_target.blend_state.src_color_blendfactor = ToSDLBlend(desc.raster.src_factor);
            color_target.blend_state.dst_color_blendfactor = ToSDLBlend(desc.raster.dst_factor);
            color_target.blend_state.color_blend_op        = SDL_GPU_BLENDOP_ADD;
            color_target.blend_state.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
            color_target.blend_state.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ZERO;
            color_target.blend_state.alpha_blend_op        = SDL_GPU_BLENDOP_ADD;
            color_target.blend_state.color_write_mask      = 0xF;
        }
    }

    SDL_GPUGraphicsPipelineTargetInfo target_info = {};
    target_info.color_target_descriptions = desc.depth_only ? nullptr : &color_target;
    target_info.num_color_targets         = desc.depth_only ? 0u : 1u;
    if (desc.has_depth_target || desc.depth_only) {
        // D32_FLOAT: must match GpuDepthTexture::Init which uses D32_FLOAT.
        // Intel Gen9 (HD 520) cannot sample D24_UNORM — pipeline must match texture.
        target_info.depth_stencil_format       = SDL_GPU_TEXTUREFORMAT_D32_FLOAT;
        target_info.has_depth_stencil_target   = true;
    }

    SDL_GPUGraphicsPipelineCreateInfo ci = {};
    ci.vertex_shader          = vert_sh;
    ci.fragment_shader        = frag_sh;
    ci.vertex_input_state     = vertex_input;
    ci.primitive_type         = ToSDLPrim(desc.raster.topology);
    ci.rasterizer_state.fill_mode  = SDL_GPU_FILLMODE_FILL;
    ci.rasterizer_state.cull_mode  = desc.raster.cull_back
                                     ? SDL_GPU_CULLMODE_BACK
                                     : SDL_GPU_CULLMODE_NONE;
    ci.rasterizer_state.front_face = SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE;
    // Guard: SDL3 asserts if compare_op==INVALID when depth_test=true.
    // Defensive fallback so callers that forget to set depth_compare_op don't crash.
    SDL_GPUCompareOp eff_compare_op = desc.raster.depth_compare_op;
    if (desc.raster.depth_test && eff_compare_op == SDL_GPU_COMPAREOP_INVALID)
        eff_compare_op = SDL_GPU_COMPAREOP_LESS_OR_EQUAL;
    ci.depth_stencil_state.compare_op         = eff_compare_op;
    ci.depth_stencil_state.enable_depth_test  = desc.raster.depth_test  ? true : false;
    ci.depth_stencil_state.enable_depth_write = desc.raster.depth_write ? true : false;
    ci.target_info = target_info;

    // Pipeline object cache: avoid recompiling identical pipeline configs.
    uint32_t raster_bits = 0;
    raster_bits |= (uint32_t)desc.raster.depth_test    << 0;
    raster_bits |= (uint32_t)desc.raster.depth_write   << 1;
    raster_bits |= (uint32_t)desc.raster.cull_back     << 2;
    raster_bits |= (uint32_t)desc.raster.blend_enable   << 3;
    raster_bits |= (uint32_t)desc.raster.depth_compare_op << 4;
    const uint32_t pipe_hash = PipeHash(desc.vert_path, desc.frag_path,
                                         desc.shader_features, raster_bits);
    SDL_GPUGraphicsPipeline* cached = PipeCache_Get(pipe_hash);
    if (cached) {
        SDL_ReleaseGPUShader(dev, vert_sh);
        SDL_ReleaseGPUShader(dev, frag_sh);
        sdl_pipeline_ = cached;
        md::GpuResourceTracker::Get().OnPipelineCreate();
        return true;
    }

    fprintf(stderr, "[PipeDiag] %s / %s  depth_test=%d compare_op=%d\n",
            desc.vert_path, desc.frag_path,
            (int)ci.depth_stencil_state.enable_depth_test,
            (int)ci.depth_stencil_state.compare_op);
    sdl_pipeline_ = SDL_CreateGPUGraphicsPipeline(dev, &ci);

    // Shaders are consumed by the pipeline; release immediately.
    SDL_ReleaseGPUShader(dev, vert_sh);
    SDL_ReleaseGPUShader(dev, frag_sh);

    if (sdl_pipeline_) PipeCache_Put(pipe_hash, sdl_pipeline_);

    if (!sdl_pipeline_) {
        MD_LOG(MD_LOG_WARNING, "[GpuPipeline] SDL_CreateGPUGraphicsPipeline failed: %s",
               SDL_GetError());
        return false;
    }
    md::GpuResourceTracker::Get().OnPipelineCreate();
    MD_LOG(MD_LOG_INFO, "[GpuPipeline] SDL_GPU pipeline created: %s / %s",
           desc.vert_path, desc.frag_path);
    return true;
#else
    (void)desc;
    return false;
#endif
}

void GpuPipeline::Destroy() {
#ifdef MD_SDL_GPU
    if (sdl_pipeline_) {
        // gaia-migration Phase 5 GATE 5 follow-up (2026-09-10): was a raw
        // SDL_ReleaseGPUGraphicsPipeline() that freed sdl_pipeline_ but left
        // its entry in the process-wide s_pipe_cache (s_pipe_cache lives in
        // monkey_dust_engine, linked into the host exe, so it survives an
        // editor_panels.so dlclose/dlopen reload cycle) -- exactly the
        // use-after-free Reload()'s own comment above already documents for
        // that path, just triggered here by any direct Destroy() caller
        // instead (e.g. PropRenderer::Shutdown()/TerrainShadingProjected::
        // Shutdown(), both called from every WorldEditor3D_SDLGPU reload).
        // On the NEXT Create() for the same vert/frag/features/raster combo,
        // PipeCache_Get() handed back the dangling pointer as if valid; a
        // LATER reload's Destroy() then double-freed it -- reproduced as a
        // real Vulkan validation error (VUID-vkDestroyPipeline-pipeline-
        // parameter, "Couldn't find VkPipeline Object") right before a
        // SIGSEGV under rapid repeated reload. MdPipeCache_Invalidate()
        // (same call Reload() already makes) both releases the GPU resource
        // AND evicts the cache entry atomically, closing the gap.
        MdPipeCache_Invalidate(desc_.vert_path, desc_.frag_path);
        sdl_pipeline_ = nullptr;
        md::GpuResourceTracker::Get().OnPipelineDestroy();
    }
#endif
}

bool GpuPipeline::Reload() {
    if (!desc_.vert_path || !desc_.frag_path) return false;
    MdSpvCache_Invalidate(desc_.vert_path);
    MdSpvCache_Invalidate(desc_.frag_path);
    // Destroy() releases sdl_pipeline_ but doesn't evict it from
    // s_pipe_cache. Without this, Create()'s hash lookup (PipeHash is
    // computed the same way from the same vert/frag path + features/raster
    // bits) finds the JUST-DESTROYED pointer still cached and hands it back
    // as if valid -- 100%-reproducible use-after-free on the very first
    // Reload() of any pipeline (e.g. every /reload-shaders hot-reload).
    MdPipeCache_Invalidate(desc_.vert_path, desc_.frag_path);
    Destroy();
    bool ok = Create(desc_);
    if (ok)
        fprintf(stdout, "[GpuPipeline] Reloaded: %s + %s\n", desc_.vert_path, desc_.frag_path);
    else
        fprintf(stderr, "[GpuPipeline] Reload FAILED: %s + %s\n", desc_.vert_path, desc_.frag_path);
    return ok;
}

int GpuPipeline::UniformLoc(const char* name) const {
    (void)name; return -1;
}

// ── GpuComputePipeline ────────────────────────────────────────────────────────

bool GpuComputePipeline::Create(const Desc& desc) {
    if (!desc.glsl_path) {
        MD_LOG(MD_LOG_WARNING, "[GpuComputePipeline] Create: null glsl_path");
        return false;
    }

#ifdef MD_SDL_GPU
    SDL_GPUDevice* dev = md::GpuDevice::Get().SDLDevice();
    if (dev) {
        char spv[256];
        MakeSpvPath(spv, sizeof(spv), desc.glsl_path);
        size_t code_size = 0;
        void*  code = ReadBinaryFile(spv, &code_size);
        if (code) {
            SDL_GPUComputePipelineCreateInfo ci = {};
            ci.code                           = (const Uint8*)code;
            ci.code_size                      = code_size;
            ci.entrypoint                     = "main";
            ci.format                         = SDL_GPU_SHADERFORMAT_SPIRV;
            ci.num_uniform_buffers            = desc.num_uniform_buffers;
            ci.num_readonly_storage_buffers   = desc.num_readonly_storage_buffers;
            ci.num_readwrite_storage_buffers  = desc.num_readwrite_storage_buffers;
            ci.num_readonly_storage_textures  = desc.num_readonly_storage_textures;
            ci.num_readwrite_storage_textures = desc.num_readwrite_storage_textures;
            ci.num_samplers                   = desc.num_samplers;
            ci.threadcount_x                  = desc.threadcount_x;
            ci.threadcount_y                  = desc.threadcount_y;
            ci.threadcount_z                  = desc.threadcount_z;
            sdl_pipeline_ = SDL_CreateGPUComputePipeline(dev, &ci);
            // Respect SPIR-V cache: only free if not cache-owned.
            if (!SpvIsCached(code)) free(code);
            if (!sdl_pipeline_)
                MD_LOG(MD_LOG_WARNING, "[GpuComputePipeline] SDL_CreateGPUComputePipeline %s: %s",
                       desc.glsl_path, SDL_GetError());
            else {
                MD_LOG(MD_LOG_INFO, "[GpuComputePipeline] SDL_GPU pipeline: %s", desc.glsl_path);
                md::GpuResourceTracker::Get().OnComputePipelineCreate();
            }
        } else {
            MD_LOG(MD_LOG_WARNING, "[GpuComputePipeline] SPIR-V not found: %s", spv);
        }
    }
    return sdl_pipeline_ != nullptr;
#else
    return false;
#endif
}

void GpuComputePipeline::Destroy() {
#ifdef MD_SDL_GPU
    if (sdl_pipeline_) {
        SDL_ReleaseGPUComputePipeline(md::GpuDevice::Get().SDLDevice(), sdl_pipeline_);
        sdl_pipeline_ = nullptr;
        md::GpuResourceTracker::Get().OnComputePipelineDestroy();
    }
#endif
}

int GpuComputePipeline::UniformLoc(const char* name) const {
    (void)name;
    return -1; // SDL_GPU: use PushUniforms instead
}

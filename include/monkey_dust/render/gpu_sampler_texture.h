#pragma once
// gpu_sampler_texture.h — GpuSamplerDesc / GpuSampler / GpuColorTexture /
// GpuTexture (split from gpu_hal.h, code audit proposal #8). GpuSamplerDesc
// is moved ahead of its original position (was forward-declared in gpu_hal.h
// because GpuSampler/GpuColorTexture appeared before it in that single file)
// so this file needs no forward declaration -- pure reordering, no behavior
// change (C++ doesn't care where an unrelated top-level type is defined
// relative to others, only that it precedes its first use, which it now does
// directly instead of via a forward decl).

#include <cstdint>

#ifdef MD_SDL_GPU
#include <SDL3/SDL_gpu.h>
#endif

struct GpuSamplerDesc {
    enum class Filter : uint8_t { NEAREST, LINEAR, LINEAR_MIPMAP };
    enum class Wrap   : uint8_t { REPEAT, CLAMP_TO_EDGE };

    Filter min_filter  = Filter::LINEAR_MIPMAP;
    Filter mag_filter  = Filter::LINEAR;
    Wrap   wrap_s      = Wrap::REPEAT;
    Wrap   wrap_t      = Wrap::REPEAT;
    bool   gen_mipmap  = false;
    bool   flip_v      = false;
    // L2-style GL2TextureDetail: +0=full, +1=half-res mip, +2=quarter-res mip.
    // Passed to SDL_GPUSamplerCreateInfo::mip_lod_bias.
    float  mip_lod_bias = 0.f;

    static GpuSamplerDesc Default()  { return {}; }
    static GpuSamplerDesc PixelArt() {
        GpuSamplerDesc s;
        s.min_filter = Filter::LINEAR_MIPMAP; s.mag_filter = Filter::NEAREST;
        s.wrap_s = Wrap::CLAMP_TO_EDGE; s.wrap_t = Wrap::CLAMP_TO_EDGE;
        s.gen_mipmap = true; s.flip_v = true;
        return s;
    }
    static GpuSamplerDesc Lut() {
        GpuSamplerDesc s;
        s.min_filter = Filter::LINEAR; s.mag_filter = Filter::LINEAR;
        s.wrap_s = Wrap::CLAMP_TO_EDGE; s.wrap_t = Wrap::CLAMP_TO_EDGE;
        return s;
    }
};

#ifdef MD_SDL_GPU
// ─────────────────────────────────────────────────────────────────────────────
// GpuSampler / GpuColorTexture — split texture/sampler ownership (SDL_GPU only).
//
// M1 HAL-closure, §3 item 9 (2026-08-31): every texture-owning class above
// (GpuTexture, GpuDepthTexture) bundles exactly one sampler with exactly one
// texture -- correct for the asset-loading/depth-attachment shapes they were
// built for, but real post-process call sites don't share that shape.
// bloom_system.cpp creates 4 render targets against 2 samplers (shared by
// filter-mode purpose, not paired to any one texture); motion_blur.cpp and
// deferred_lighting.cpp have the same N-textures-vs-M-samplers pattern.
// Forcing these through GpuTexture would silently create extra sampler
// objects the original code never had -- a real resource-count divergence
// invisible to pixel-diff A/B (the same class of gap named in this file's
// error-path lesson, docs/HAL_CLOSURE_PROGRESS.md). These two classes are
// deliberately NOT coupled to each other; callers own however many of each
// their real binding pattern needs.
class GpuSampler {
public:
    bool Init(const GpuSamplerDesc& desc);
    void Shutdown();
    SDL_GPUSampler* SDLSampler() const { return sdl_sampler_; }
private:
    SDL_GPUSampler* sdl_sampler_ = nullptr;
};

class GpuColorTexture {
public:
    bool Init(int w, int h, SDL_GPUTextureFormat format, SDL_GPUTextureUsageFlags usage,
              uint32_t num_levels = 1);
    void Shutdown();
    // TEMPORARY raw-SDL bridge, same idiom as GpuDevice::SDLDevice() --
    // see its comment. Do not add new call sites.
    SDL_GPUTexture* SDLTexture() const { return sdl_tex_; }
    int Width()  const { return w_; }
    int Height() const { return h_; }
private:
    SDL_GPUTexture* sdl_tex_ = nullptr;
    int w_ = 0, h_ = 0;
};
#endif // MD_SDL_GPU

// ─────────────────────────────────────────────────────────────────────────────
// GpuTexture — RGBA8 texture + sampler.
// OpenGL: glGenTextures + glTexImage2D + sampler params.
// SDL_GPU: SDL_CreateGPUTexture(R8G8B8A8_UNORM) + SDL_CreateGPUSampler +
//   one-shot SDL_UploadToGPUTexture + optional SDL_GenerateMipmapsForGPUTexture.
// ─────────────────────────────────────────────────────────────────────────────
class GpuTexture {
public:
    bool InitFromFile  (const char* path,              const GpuSamplerDesc& s = {});
    // Uncompressed (DDPF_RGB, NOT BC1/BC3) single 2D DDS texture — no
    // zlib/DEFLATE decode, same rationale as world_hmap.r16 vs PNG16 (see
    // tools/md_hmap_io.py's doc comment): for large flat textures (e.g. the
    // 16384x16384 Kenshi terrain colour atlas), stb_image's PNG decode cost
    // dominates startup (measured ~17s for that one file) while a plain
    // fread is a fraction of that. Chosen as a real DDS (not a custom raw
    // format) so the file stays openable in standard tools (RenderDoc,
    // GIMP's DDS plugin, texconv) for visual sanity-checking — see
    // tools/md_stitch_terrain.py's dds_header_uncompressed_rgba() for the
    // exact header this reads (masks R=0x000000FF G=0x0000FF00 B=0x00FF0000
    // A=0xFF000000, i.e. already in this class's native RGBA8 byte order —
    // pixel data is a direct fread with no expand/swizzle pass, same as the
    // earlier from-scratch raw-format version that measured ~1.9-2.5s here
    // (vs ~10.6s for an RGB-then-expand version, vs ~17.3s for PNG).
    // Dispatched automatically by InitFromFile() when path ends in ".dds"
    // and the pixelformat is DDPF_RGB (as opposed to the existing BC1/BC3
    // array loader, InitFromDDSArray, which stays untouched).
    bool InitFromDDS   (const char* path,              const GpuSamplerDesc& s = {});
    bool InitFromMemory(const uint8_t* rgba8, int w, int h, const GpuSamplerDesc& s = {});
    // Allocates a COLOR_TARGET|SAMPLER texture with NO data upload (SDL_GPU
    // only) — for render-to-texture targets whose first write is a render
    // pass, not CPU data (e.g. TerrainRenderer::BakeAlbedo). InitFromMemory's
    // zero-fill-then-upload does its own synchronous acquire/copy/submit
    // cycle per call; calling it once per chunk (81+) measurably regressed
    // startup time (confirmed: ~4s baseline -> ~16s) for content that gets
    // overwritten by the very next render pass anyway. mip levels (if
    // s.gen_mipmap) are left uninitialized until the caller's own render
    // pass + SDL_GenerateMipmapsForGPUTexture populate them.
    // format: SDL_GPU only, INVALID(0) = R8G8B8A8_UNORM (previous hardcoded
    // default, unchanged for existing callers -- this parameter had zero
    // real call sites before TERRAIN_CA_REBUILD_PROMPT.md Phase 4's
    // screen-space G-buffer target needed full-float world-space storage,
    // which R8G8B8A8_UNORM's normalized [0,1] range cannot represent).
#ifdef MD_SDL_GPU
    bool InitRenderTarget(int w, int h, const GpuSamplerDesc& s = {},
                          SDL_GPUTextureFormat format = SDL_GPU_TEXTUREFORMAT_INVALID);
    // Compute-storage-writable texture (SDL_GPU only) -- InitRenderTarget
    // above always ORs in COLOR_TARGET|SAMPLER, which doesn't fit: real
    // compute-write call sites need a caller-specified usage combination
    // (M1 HAL-closure, §3 item 9, 2026-08-31 -- 3 confirmed real shapes,
    // not one: ssao_system.cpp's ao_tex_ and terrain_vt_page_cache.cpp's
    // atlas_tex_ are COMPUTE_STORAGE_WRITE|SAMPLER with no COLOR_TARGET;
    // terrain_world_heightmap.cpp's normal_tex_ additionally ORs in
    // COLOR_TARGET because it's the target of its own
    // SDL_GenerateMipmapsForGPUTexture call). num_levels is an explicit
    // caller-computed value, not derived from GpuSamplerDesc::gen_mipmap
    // via MipLevels(w,h) the way InitRenderTarget's is, because
    // normal_tex_'s real mip-level count is computed differently (from N,
    // not from w/h via the shared power-of-two halving helper) and forcing
    // it through the same derivation risked a silent off-by-one in the mip
    // chain length -- exactly the class of divergence pixel-diff A/B can't
    // catch (see docs/HAL_CLOSURE_PROGRESS.md's error-path methodology
    // lesson for the general form of this risk).
    bool InitCompute(int w, int h, SDL_GPUTextureFormat format,
                      SDL_GPUTextureUsageFlags usage, uint32_t num_levels,
                      const GpuSamplerDesc& s = {});
#else
    bool InitRenderTarget(int w, int h, const GpuSamplerDesc& s = {});
#endif
    // Load a 2D texture array from multiple BC3/DXT5 DDS files (SDL_GPU only).
    // All files must have identical dimensions, format, and mip count.
    bool InitFromDDSArray(const char* const* paths, int count, const GpuSamplerDesc& s = {});
    void Shutdown();

    // OpenGL: glActiveTexture + glBindTexture.
    // SDL_GPU: binding via SDL_BindGPUFragmentSamplers in render pass (Step 6).
    void Bind(uint32_t unit) const;

    int          Width()     const { return w_; }
    int          Height()    const { return h_; }
    unsigned int GLTexture() const { return id_; }
    bool Valid() const {
#ifdef MD_SDL_GPU
        return sdl_tex_ != nullptr || id_ != 0;
#else
        return id_ != 0;
#endif
    }
#ifdef MD_SDL_GPU
    // TEMPORARY raw-SDL bridge, same idiom as GpuDevice::SDLDevice() --
    // see its comment. Do not add new call sites.
    SDL_GPUTexture* SDLTexture() const { return sdl_tex_; }
    SDL_GPUSampler* SDLSampler() const { return sdl_sampler_; }
    // Transfer SDL_GPU ownership out of this object (caller is responsible for release).
    SDL_GPUTexture* TakeSDLTexture() { SDL_GPUTexture* t = sdl_tex_; sdl_tex_ = nullptr; return t; }
    SDL_GPUSampler* TakeSDLSampler() { SDL_GPUSampler* s = sdl_sampler_; sdl_sampler_ = nullptr; return s; }
#endif

    // Transfer GL texture ownership.
    unsigned int Release() { unsigned int t = id_; id_ = 0; return t; }

private:
    void ApplySampler(const GpuSamplerDesc& s) const; // OpenGL only
    unsigned int id_ = 0;
    int w_ = 0, h_ = 0;
#ifdef MD_SDL_GPU
    SDL_GPUTexture* sdl_tex_     = nullptr;
    SDL_GPUSampler* sdl_sampler_ = nullptr;
#endif
};

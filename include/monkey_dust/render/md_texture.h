#pragma once
// MdTexture — texture loader using stb_image (no Raylib).
// Supported backends: OpenGL 4.3 (MD_OPENGL43_ENABLED) and SDL_GPU (MD_SDL_GPU).
// Under neither flag all functions are no-ops / return {}.
//
// Implementation lives in md_texture.cpp.

#include <cstdint>

// IMPORTANT: layout must NOT change between MD_SDL_GPU and non-SDL_GPU builds.
// Engine lib is always compiled with MD_SDL_GPU; editor/tools may omit it.
// Mismatched struct sizes cause ABI breakage (wrong array offsets, bad returns).
struct MdTexture {
    unsigned int id = 0;           // OpenGL texture name (0 in SDL_GPU path)
    int w = 0, h = 0;
    void* sdl_tex     = nullptr;   // SDL_GPUTexture*  (null when not using SDL_GPU)
    void* sdl_sampler = nullptr;   // SDL_GPUSampler*  (null when not using SDL_GPU)
};


// Load RGBA texture from file via stb_image.
// mip_lod_bias: L2 GL2TextureDetail — 0=full, +1=half-res mip, +2=quarter-res.
MdTexture MdLoadTexture(const char* path, float mip_lod_bias = 0.f);

// Load RGBA texture optimized for pixel-art rendering:
// GL_NEAREST filter + GL_CLAMP_TO_EDGE + mipmap (no seam bleeding / aliasing).
// Use for: Flare sprite atlases, tile sheets, any pixel-art content.
MdTexture MdLoadTexturePixelArt(const char* path);

// Upload raw RGBA8 pixel data (for procedural textures such as BRDF LUT).
MdTexture MdLoadTextureFromMemory(const uint8_t* data, int w, int h);

#ifdef MD_SDL_GPU
#include <monkey_dust/render/gpu_device.h>
#include <monkey_dust/render/gpu_sampler_texture.h>
// 2026-09-05 (simplification audit, dead-duplicate finding): upload a
// square RGBA8 pixel buffer directly into a caller-owned GpuColorTexture
// (not MdTexture -- callers that already keep a GpuColorTexture member,
// e.g. a UI icon slot, don't need the GL+SDL_GPU dual-handle wrapper).
// Was two independent, DIVERGED copies (game/src/render/scene_render.cpp's
// SceneRender::MakeCamTex and tools/flare_demo/flare_demo_recording.cpp's
// free-function MakeCamTex, both size=72 by coincidence, not by shared
// constant) -- the scene_render.cpp copy had gained null-checks on
// AcquireCommandBuffer()/cp.SDLPass() that the flare_demo copy never
// received. Consolidated here so a future fix lands once, not per-copy.
bool MdUploadSquareRGBA8ToGpuColorTexture(md::GpuDeviceHandle dev, const uint8_t* pixels,
                                            int size, GpuColorTexture& out);
#endif // MD_SDL_GPU

void MdUnloadTexture(MdTexture& t);
void MdBindTexture  (MdTexture t, int unit);  // OpenGL only; no-op in SDL_GPU-only builds

// ── Texture cache ─────────────────────────────────────────────────────────────
// MdLoadTexture / MdLoadTexturePixelArt cache results by FNV-1a path hash.
// Repeated loads of the same path return the cached MdTexture without hitting disk.
// MdUnloadTexture on a cached texture zeroes the caller's copy; GPU resources stay
// alive until MdTextureCache_Shutdown() is called (typically at app exit).
//
// Max capacity: 128 entries per variant (pixel-art / linear).
// Call MdTextureCache_Shutdown() once before SDL_GPU device destruction.
void MdTextureCache_Shutdown();

// Returns total number of cached entries (pixel-art + linear combined).
// Writes per-variant counts if out_pa / out_lin are non-null.
int  MdTextureCache_Stats(int* out_pa = nullptr, int* out_lin = nullptr);


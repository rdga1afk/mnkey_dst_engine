#pragma once
// MdTexture — texture loader using stb_image (no Raylib).
// Backend: SDL_GPU only (the OpenGL path was removed in #185 K2).
//
// Implementation lives in md_texture.cpp.

#include <cstdint>

// IMPORTANT: layout must stay identical in every binary that includes this header
// (mismatched struct sizes cause ABI breakage: wrong array offsets, bad returns).
struct MdTexture {
    unsigned int id = 0;           // OpenGL texture name (0 in SDL_GPU path)
    int w = 0, h = 0;
    void* sdl_tex     = nullptr;   // SDL_GPUTexture*  (null when not using SDL_GPU)
    void* sdl_sampler = nullptr;   // SDL_GPUSampler*  (null when not using SDL_GPU)
};


// Load RGBA texture from file via stb_image.
// mip_lod_bias: L2 GL2TextureDetail — 0=full, +1=half-res mip, +2=quarter-res.
MdTexture MdLoadTexture(const char* path, float mip_lod_bias = 0.f);


// Upload raw RGBA8 pixel data (for procedural textures such as BRDF LUT).
MdTexture MdLoadTextureFromMemory(const uint8_t* data, int w, int h);

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

void MdUnloadTexture(MdTexture& t);




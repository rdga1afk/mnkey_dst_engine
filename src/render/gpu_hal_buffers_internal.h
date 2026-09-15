#pragma once
// Shared preamble + cross-TU declarations for the gpu_hal_buffers.cpp split
// (GpuVertexBuffer/GpuDepthTexture/GpuStaticBuffer/GpuUploadBatch/GpuTexture
// — one engine class per file, GpuTexture spread across several since it has
// the most methods). MipLevels/CreateSDLSampler/s_bc_mip_bytes were
// originally file-static helpers shared across several GpuTexture methods
// that now live in different TUs — given external linkage here so each
// fragment can still call them, mirroring the pattern used for
// LogicTickJobCtx's cross-TU declarations in game/src/logic_tick_internal.h.

#include <monkey_dust/render/gpu_pipeline.h>
#include <monkey_dust/render/gpu_depth_texture.h>
#include <monkey_dust/render/gpu_sampler_texture.h>
#include <monkey_dust/render/gpu_static_buffer.h>
#include <monkey_dust/render/gpu_resource_tracker.h>
#include <monkey_dust/platform/md_log.h>
#include <monkey_dust/platform/md_fs.h>
#include <monkey_dust/platform/job_system.h>
#include <cstdlib>
#include <cstdio>
#include <cstring>

#include "glad.h"
#include "stb_image.h"

#ifdef MD_SDL_GPU
#include <monkey_dust/render/gpu_device.h>

uint32_t MipLevels(int w, int h);
SDL_GPUSampler* CreateSDLSampler(SDL_GPUDevice* dev, const GpuSamplerDesc& s);
#endif

// Per-mip BC-compressed byte size (4×4 texel blocks) — shared by
// GpuTexture::InitFromDDS (single texture) and InitFromDDSArray (layered).
size_t s_bc_mip_bytes(int w, int h, int bytes_per_block);

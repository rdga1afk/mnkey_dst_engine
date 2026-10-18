#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// GPU Hardware Abstraction Layer — dual-backend (OpenGL 4.3 / SDL_GPU).
//
// API design mirrors SDL_GPU so future backend swap is mechanical:
//
//   GpuPipeline       → SDL_CreateGPUGraphicsPipeline
//   GpuVertexBuffer   → SDL_GPUBuffer (device) + SDL_GPUTransferBuffer (staging)
//   GpuCommandBuffer  → SDL_GPUCommandBuffer + SDL_BeginGPURenderPass
//
// Backend selection:
//   SDL_GPU is the only path (the OpenGL path was removed in #185 K2).
//
// Code audit proposal #8 (docs/CODE_AUDIT_2026-09.md, 2026-09): this file
// was ~1200 lines / ~16 classes. Split into gpu_hal_*.h / gpu_*.h below by
// cohesive group (not literal 1:1 class:file -- GpuPassView/GpuRenderPass
// stay together since one constructs the other; GpuSamplerDesc/GpuSampler/
// GpuColorTexture/GpuTexture stay together as one texture/sampler group).
// This header is now a facade: every one of the ~70 existing
// `#include <monkey_dust/render/gpu_hal.h>` call sites keeps working
// unchanged, in the SAME declaration order as the original single file, so
// splitting carries zero call-site risk.
// ─────────────────────────────────────────────────────────────────────────────

#include <monkey_dust/render/gpu_hal_types.h>
#include <monkey_dust/render/gpu_pipeline.h>
#include <monkey_dust/render/gpu_compute.h>
#include <monkey_dust/render/gpu_copy_pass.h>
#include <monkey_dust/render/gpu_depth_texture.h>
#include <monkey_dust/render/gpu_sampler_texture.h>
#include <monkey_dust/render/gpu_static_buffer.h>
#include <monkey_dust/render/gpu_pass_view.h>
#include <monkey_dust/render/gpu_hal_free_functions.h>

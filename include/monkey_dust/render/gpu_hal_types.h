#pragma once
// gpu_hal_types.h — shared basic types for the GPU HAL (split from gpu_hal.h,
// code audit proposal #8: docs/CODE_AUDIT_2026-09.md). Topology/format/blend
// enums and vertex/raster descriptors used across every other gpu_hal_*.h
// file. Always included first by gpu_hal.h's facade.

#include <cstdint>

#ifdef MD_SDL_GPU
#include <SDL3/SDL_gpu.h>
#endif

// ── Vertex topology ───────────────────────────────────────────────────────────
enum class GpuTopology : uint8_t { TRIANGLES, POINTS, LINES };

// ── Vertex attribute format ───────────────────────────────────────────────────
enum class GpuAttribFmt : uint8_t {
    F1, F2, F3, F4,       // float scalars / vectors
    U8x4_NORM,            // uint8×4 normalized [0,1] → vec4
    U8x4                  // uint8×4 raw (bone joint indices → uvec4)
};

// ── Blend factor (portable; maps to GL and SDL_GPU) ───────────────────────────
enum class GpuBlendFactor : uint8_t {
    ZERO,
    ONE,
    SRC_COLOR,
    ONE_MINUS_SRC_COLOR,
    SRC_ALPHA,
    ONE_MINUS_SRC_ALPHA,
    DST_ALPHA,
    ONE_MINUS_DST_ALPHA,
    DST_COLOR,
    ONE_MINUS_DST_COLOR,
};

// ── Single vertex attribute descriptor ───────────────────────────────────────
struct GpuVertexAttrib {
    uint32_t    location;  // shader `layout(location = N) in`
    uint32_t    offset;    // byte offset within one vertex
    GpuAttribFmt fmt;
};

// ── Vertex buffer layout ──────────────────────────────────────────────────────
// slot 0: per-vertex attributes (stride > 0 required).
// slot 1: second buffer — per-instance (inst_per_vertex=false, default) OR
//         per-vertex (inst_per_vertex=true) when inst_stride > 0 && inst_count > 0.
struct GpuVertexLayout {
    GpuVertexAttrib attribs[8] = {};
    uint32_t        count      = 0;
    uint32_t        stride     = 0;   // bytes per vertex (slot 0)
    // Optional second binding (slot 1).
    GpuVertexAttrib inst_attribs[8] = {};
    uint32_t        inst_count      = 0;
    uint32_t        inst_stride     = 0;    // bytes per element; 0 = disabled
    bool            inst_per_vertex = false; // true = VERTEX rate; false = INSTANCE rate
};

// ── Rasterizer / output-merger state (immutable in pipeline) ─────────────────
struct GpuRasterState {
    GpuTopology   topology    = GpuTopology::TRIANGLES;
    bool          blend_enable = false;
    GpuBlendFactor src_factor  = GpuBlendFactor::SRC_ALPHA;
    GpuBlendFactor dst_factor  = GpuBlendFactor::ONE_MINUS_SRC_ALPHA;
    bool  depth_test           = true;
    bool  depth_write          = true;
    bool  cull_back            = true;
    bool  point_size           = false; // GL_PROGRAM_POINT_SIZE
    // SDL_GPU_FILLMODE_LINE instead of FILL -- debug wireframe overlay.
    // Requires the driver's fillModeNonSolid feature; confirmed present on
    // this project's target hardware (Intel Gen9 ANV).
    bool  wireframe            = false;
#ifdef MD_SDL_GPU
    // Depth compare function. Default LESS_OR_EQUAL matches the prepass write.
    // Set to EQUAL for main pass after an Early-Z prepass (depth_write=false).
    SDL_GPUCompareOp depth_compare_op = SDL_GPU_COMPAREOP_LESS_OR_EQUAL;
#endif
};

// ── ShaderFeature — SPIR-V variant selection ──────────────────────────────────
// Vulkan adaptation: replaces VkSpecializationInfo with pre-compiled .spv variants.
// Each bit selects compile-time constants via glslc -D, producing optimized SPIR-V
// (dead code elimination, loop unrolling) equivalent to VkSpecializationInfo.
//
// Naming convention: basename + suffix per active flag, e.g.:
//   SF_None                      → "mesh.spv"
//   SF_Skinned                   → "mesh_skinned.spv"
//   SF_Skinned | SF_Shadows      → "mesh_skinned_shadow.spv"
//   SF_Shadows | SF_PCF_HIGH     → "mesh_shadow_pcfhigh.spv"
// Variants must be pre-compiled by scripts/compile_shaders.sh.
enum ShaderFeature : uint32_t {
    SF_None      = 0,
    SF_Skinned   = 1u << 0,  // GPU skeletal animation via SSBO bones (binding 4)
    SF_Shadows   = 1u << 1,  // CSM shadow sampling (3 cascades)
    SF_AlphaTest = 1u << 2,  // frag discard for alpha-tested geometry
    // VBfA-R9: character material variants (same animated.vert, different FS)
    SF_Emissive  = 1u << 3,  // animated_emissive.frag — glowing NPC (fire/magic)
    SF_Dissolve  = 1u << 4,  // animated_dissolve.frag — death/despawn noise clip
    SF_Ice       = 1u << 5,  // animated_ice.frag (future: frozen NPC effect)
    // Specialization constant variants (glslc -DFLAG=1):
    SF_PCF_HIGH  = 1u << 6,  // -DPCF_SAMPLES=16  (vs default 4) — higher shadow quality
    SF_SSAO_INT  = 1u << 7,  // -DSSAO_INTEGRATION — forward pass reads SSAO buffer
    SF_IBL       = 1u << 8,  // -DIBL_ENABLED — Image-Based Lighting for PBR materials
    SF_DITHERED  = 1u << 9,  // -DDITHERED_TRANSPARENCY — ordered dithering alpha
};

#pragma once
// GpuPipelineSafetyCheck — static validation of GpuPipeline::Desc /
// GpuComputePipeline::Desc resource combinations against known-unsafe
// patterns on this project's target hardware (Intel Gen9 ANV), BEFORE
// Create() is called.
//
// docs/DAGOR_IMPLEMENTATION_PROMPT.md Рівень 2, КРОК 6 -- addresses the
// bug CLASS behind task #141's two real GpuComputePipeline::Create
// segfaults (journalctl-confirmed crashes inside libvulkan_intel.so,
// see terrain_zone_corner_bake.comp's own doc comment for the full
// story): mixing a storage BUFFER with samplers/storage-TEXTURES in one
// compute pipeline crashes the driver, with no Vulkan validation error
// to catch it -- every working compute pipeline in this codebase uses
// EITHER (samplers + a storage texture + UBO) [terrain_worldmap_
// normal_bake.comp, terrain_zone_corner_bake.comp's fixed form] OR
// (storage buffers + UBO, no samplers/textures) [npc_cull.comp], never
// both categories together.
//
// This is a compile-time-adjacent SAFETY NET, not a full resource
// scheduler (that would be a much larger, daFrameGraph-style project --
// explicitly out of scope, see docs/DAGOR_ANALYSIS_2026-09.md's "чого
// НЕ можемо" table). Call Validate*() before Create() and treat a
// `false` return as "stop and redesign the pipeline shape", not
// something to silently work around.

#include <monkey_dust/render/gpu_pipeline.h>
#include <monkey_dust/render/gpu_compute.h>

namespace md {

// Reason string is a static literal (safe to store/print, never freed).
// Returns true (safe) with *out_reason left untouched if desc has no
// known-unsafe combination on record.
inline bool ValidateComputePipelineDesc(const ::GpuComputePipeline::Desc& desc,
                                         const char** out_reason) {
    const bool hasStorageBuffer = desc.num_readonly_storage_buffers > 0 ||
                                   desc.num_readwrite_storage_buffers > 0;
    const bool hasSamplerOrTexture = desc.num_samplers > 0 ||
                                      desc.num_readonly_storage_textures > 0 ||
                                      desc.num_readwrite_storage_textures > 0;
    // The actual, journalctl-confirmed root cause of task #141's two
    // real crashes (terrain_zone_corner_bake.comp doc comment) -- fixed
    // there by moving the storage-buffer data into a small sampled
    // texture instead. No exception to this rule exists anywhere in the
    // codebase today.
    if (hasStorageBuffer && hasSamplerOrTexture) {
        *out_reason =
            "storage buffer (readonly or read-write) combined with samplers/"
            "storage-textures in one compute pipeline crashes libvulkan_intel.so "
            "on Intel Gen9 ANV (task #141, journalctl-confirmed) -- move the "
            "buffer's data into a small sampled texture instead, or split into "
            "separate dispatches";
        return false;
    }
    // Weaker signal: >1 read-write storage texture was ALSO tried and
    // initially suspected as the cause during task #141's investigation
    // -- ruled out as the actual root cause (the storage-buffer mixing
    // above was), but no working pipeline in this codebase has ever
    // used more than 1, so this is "no known-safe precedent", not a
    // confirmed crash -- flagged separately so callers can tell the two
    // apart (out_reason text says so explicitly).
    if (desc.num_readwrite_storage_textures > 1) {
        *out_reason =
            "no working compute pipeline in this codebase uses more than 1 "
            "read-write storage texture -- NOT a confirmed crash (that turned "
            "out to be the storage-buffer/sampler mixing above), but untested; "
            "split into separate dispatches (one rw-texture each) unless you "
            "have live-verified this specific combination works";
        return false;
    }
    return true;
}

// Graphics-pipeline counterpart: catches the OTHER documented Intel
// Gen9 ANV footgun (.claude/rules/gpu-shader-debug.md's checklist) --
// fragment samplers combined with vertex-stage storage buffers is a
// SILENT failure (wrong/garbage sampling results), not a crash, which
// makes it worse to debug live -- catch it here instead.
inline bool ValidateGraphicsPipelineDesc(const ::GpuPipeline::Desc& desc,
                                          const char** out_reason) {
#ifdef MD_SDL_GPU
    if (desc.frag_samplers > 0 && desc.vert_storage_bufs > 0) {
        *out_reason =
            "frag_samplers>0 combined with vert_storage_bufs>0 silently fails "
            "on Intel Gen9 ANV (.claude/rules/gpu-shader-debug.md checklist) -- "
            "no Vulkan validation error, just wrong sampling results";
        return false;
    }
#else
    (void)desc;
#endif
    (void)out_reason;
    return true;
}

} // namespace md

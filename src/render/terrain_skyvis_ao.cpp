#ifdef MD_SDL_GPU
#include <monkey_dust/render/terrain_skyvis_ao.h>
#include <monkey_dust/render/gpu_hal_free_functions.h>
#include <monkey_dust/render/gpu_hal_types.h>
#include <monkey_dust/platform/md_log.h>
#include <cmath>

// КРОК 7 (docs/KROK7_SKYVIS_AO_DESIGN.md): implementation. Mirrors
// SSAOSystem's own .cpp structure (engine/src/render/ssao_system.cpp) --
// raw GpuCreateTexture/GpuCreateSampler for the probe texture (not the
// GpuTexture wrapper class), GpuComputePipeline for the horizon-sample
// dispatch, GpuPipeline for the fullscreen multiply-blend apply pass.

namespace {
// Field order must match shaders/terrain_skyvis_probe_update.comp's
// SkyVisUBO exactly (std140).
struct SkyVisUpdateUBO {
    float hmParams[4]; // x=worldExtent_m, y=resTexels, z=heightRange_m, w=heightMin_m
    float params[4];   // x=spacing, y=gridSize, z=region world_texel_from.x, w=.y
    float params2[4];  // x=viewport_lt.x, y=viewport_lt.y, z=radius, w=numDirs
    float params3[4];  // x=region width, y=region height, z/w unused
};
static_assert(sizeof(SkyVisUpdateUBO) == 64, "must match .comp std140 layout");

// Field order must match shaders/terrain_skyvis_apply.frag's
// SkyVisApplyUBO exactly (std140).
struct SkyVisApplyUBO {
    float probeParams[4]; // x=spacing, y=gridSize, z=mainOrigin.x, w=mainOrigin.y
    float camParams[4];   // x=camWorldX, y=camWorldZ, z=fadeRadius, w=unused
};
static_assert(sizeof(SkyVisApplyUBO) == 32, "must match .frag std140 layout");
} // namespace

bool TerrainSkyVisAO::Init(md::GpuDeviceHandle dev) {
    dev_ = dev;

    // Probe texture: R8_UNORM, kProbeGridSize^2 -- written by the update
    // compute pass (readwrite storage), read by the apply pass (sampler).
    {
        SDL_GPUTextureCreateInfo ti = {};
        ti.type                 = SDL_GPU_TEXTURETYPE_2D;
        ti.width                = (Uint32)kProbeGridSize;
        ti.height                = (Uint32)kProbeGridSize;
        ti.layer_count_or_depth = 1;
        ti.num_levels           = 1;
        ti.format               = SDL_GPU_TEXTUREFORMAT_R8_UNORM;
        ti.usage                = SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_WRITE
                                 | SDL_GPU_TEXTUREUSAGE_SAMPLER;
        probe_tex_ = GpuCreateTexture(dev, &ti);
        if (!probe_tex_) {
            MD_LOG(MD_LOG_WARNING, "TerrainSkyVisAO: probe_tex create failed: %s", SDL_GetError());
            return false;
        }
    }
    {
        SDL_GPUSamplerCreateInfo si = {};
        si.min_filter     = SDL_GPU_FILTER_LINEAR;
        si.mag_filter     = SDL_GPU_FILTER_LINEAR;
        si.mipmap_mode    = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
        si.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
        si.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
        si.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
        probe_sampler_ = GpuCreateSampler(dev, &si);
        if (!probe_sampler_) {
            MD_LOG(MD_LOG_WARNING, "TerrainSkyVisAO: probe_sampler create failed: %s", SDL_GetError());
            return false;
        }
    }

    // Update compute pipeline -- КРОК 6 safe shape: samplers(1) + ONE
    // rw-storage-texture + UBO, no storage buffer (gpu_pipeline_safety_check.h).
    {
        GpuComputePipeline::Desc pd{};
        pd.glsl_path                       = "shaders/terrain_skyvis_probe_update.comp";
        pd.num_uniform_buffers             = 1;
        pd.num_samplers                    = 1;
        pd.num_readwrite_storage_textures  = 1;
        pd.threadcount_x = 8; pd.threadcount_y = 8; pd.threadcount_z = 1;
        if (!update_pipeline_.Create(pd)) {
            MD_LOG(MD_LOG_WARNING, "TerrainSkyVisAO: update pipeline create failed");
            return false;
        }
    }

    // Apply pipeline -- fullscreen multiply-blend onto the swapchain,
    // same blend factors as SSAOSystem's own apply_pipeline_.
    {
        GpuPipeline::Desc d;
        d.vert_path           = "shaders/deferred_lighting.vert";
        d.frag_path           = "shaders/terrain_skyvis_apply.frag";
        d.layout.count        = 0;
        d.layout.stride       = 0;
        d.raster.blend_enable = true;
        d.raster.src_factor   = GpuBlendFactor::SRC_ALPHA;
        d.raster.dst_factor   = GpuBlendFactor::ONE_MINUS_SRC_ALPHA;
        d.raster.depth_test   = false;
        d.raster.depth_write  = false;
        d.raster.cull_back    = false;
        d.frag_samplers       = 3; // probeTex, gbufPacked, gbufDepth
        d.frag_uniform_bufs   = 1; // SkyVisApplyUBO
        d.has_depth_target    = false;
        d.color_format        = SDL_GPU_TEXTUREFORMAT_INVALID; // swapchain format
        if (!apply_pipeline_.Create(d)) {
            MD_LOG(MD_LOG_WARNING, "TerrainSkyVisAO: apply pipeline create failed");
            return false;
        }
    }

    origin_initialized_ = false;
    enabled_ = true;
    MD_LOG(MD_LOG_INFO, "TerrainSkyVisAO: ready (%dx%d probes, %.0fm spacing, %.0fm window)",
           kProbeGridSize, kProbeGridSize, kProbeSpacingM, kWindowM);
    return true;
}

void TerrainSkyVisAO::UpdatePass(md::GpuCommandBufferHandle cmd, float world_x, float world_z,
                                  md::GpuTextureHandle heightmap_tex, SDL_GPUSampler* heightmap_sampler,
                                  float heightmap_world_extent, float heightmap_height_min,
                                  float heightmap_height_max, int heightmap_resolution) {
    if (!enabled_ || !probe_tex_ || !update_pipeline_.SDLComputePipeline()) return;
    if (!heightmap_tex || !heightmap_sampler) return;

    last_cam_x_ = world_x;
    last_cam_z_ = world_z;

    md::IPoint2 new_origin{
        (int)std::floor(world_x / kProbeSpacingM),
        (int)std::floor(world_z / kProbeSpacingM)};

    if (!origin_initialized_) {
        // Force a full-grid invalidation on the very first call -- see
        // ToroidalUpdate's threshold param below (kProbeGridSize):
        // starting cur_origin_ exactly kProbeGridSize texels away from
        // new_origin guarantees maxMovement >= threshold, taking the
        // "invalidate everything as one region" branch instead of the
        // (here meaningless, since nothing is cached yet) partial-quad path.
        cur_origin_ = new_origin + md::IPoint2(kProbeGridSize, kProbeGridSize);
        main_origin_ = cur_origin_;
        origin_initialized_ = true;
    }

    md::ToroidalQuadRegion regions[4];
    int n = md::ToroidalUpdate(new_origin, &cur_origin_, &main_origin_,
                                kProbeGridSize, kProbeGridSize, regions);
    if (n <= 0) return; // camera hasn't moved a full probe-texel yet

    for (int i = 0; i < n; ++i) {
        const md::ToroidalQuadRegion& r = regions[i];
        if (r.size.x <= 0 || r.size.y <= 0) continue;

        GpuComputeStorageBindings sb;
        sb.cmd = cmd;
        sb.rw_textures[0] = { probe_tex_, false };
        sb.num_rw_textures = 1;

        GpuComputePass pass;
        pass.Begin(&update_pipeline_, sb);
        if (!pass.SDLPass()) continue;

        SDL_GPUTextureSamplerBinding hsamp{};
        hsamp.texture = heightmap_tex;
        hsamp.sampler = heightmap_sampler;
        pass.BindSamplers(0, &hsamp, 1);

        SkyVisUpdateUBO ubo{};
        ubo.hmParams[0] = heightmap_world_extent;
        ubo.hmParams[1] = (float)heightmap_resolution;
        ubo.hmParams[2] = heightmap_height_max - heightmap_height_min;
        ubo.hmParams[3] = heightmap_height_min;
        ubo.params[0] = kProbeSpacingM;
        ubo.params[1] = (float)kProbeGridSize;
        ubo.params[2] = (float)r.world_texel_from.x;
        ubo.params[3] = (float)r.world_texel_from.y;
        ubo.params2[0] = (float)r.viewport_lt.x;
        ubo.params2[1] = (float)r.viewport_lt.y;
        ubo.params2[2] = 24.0f; // sample radius, metres -- contact-AO scale
        ubo.params2[3] = 8.0f;  // numDirs, per design doc
        ubo.params3[0] = (float)r.size.x;
        ubo.params3[1] = (float)r.size.y;
        pass.PushUniforms(0, &ubo, sizeof(ubo));

        uint32_t gx = (uint32_t)((r.size.x + 7) / 8);
        uint32_t gy = (uint32_t)((r.size.y + 7) / 8);
        pass.Dispatch(gx, gy, 1);
        pass.End();
    }
}

void TerrainSkyVisAO::ApplyPass(md::GpuCommandBufferHandle cmd, md::GpuTextureHandle swapchain_tex,
                                 int sw, int sh, md::GpuTextureHandle gbuf_packed, SDL_GPUSampler* gbuf_sampler,
                                 md::GpuTextureHandle gbuf_depth, SDL_GPUSampler* gbuf_depth_sampler) {
    if (!enabled_ || !probe_tex_ || !apply_pipeline_.SDLPipeline()) return;
    if (!swapchain_tex || !gbuf_packed || !gbuf_depth) return;

    GpuCommandBuffer cb;
    GpuCommandBuffer::ColorPassDesc cpd;
    cpd.cmd        = cmd;
    cpd.color_tex[0] = swapchain_tex;
    cpd.load_color = true; // LOAD, preserve existing scene (same as SSAOSystem::ApplyPass)
    cb.BeginColorPass(cpd);
    if (!cb.SDLPass()) return;
    SDL_GPURenderPass* pass = cb.SDLPass();

    cb.BindPipeline(&apply_pipeline_);

    SDL_GPUTextureSamplerBinding sbs[3] = {
        { probe_tex_,  probe_sampler_ },
        { gbuf_packed, gbuf_sampler },
        { gbuf_depth,  gbuf_depth_sampler },
    };
    cb.BindFragmentSamplers(0, sbs, 3);

    SkyVisApplyUBO ubo{};
    ubo.probeParams[0] = kProbeSpacingM;
    ubo.probeParams[1] = (float)kProbeGridSize;
    ubo.probeParams[2] = (float)main_origin_.x;
    ubo.probeParams[3] = (float)main_origin_.y;
    ubo.camParams[0] = last_cam_x_;
    ubo.camParams[1] = last_cam_z_;
    // Fade band starts at 80% of the half-window (see terrain_skyvis_
    // apply.frag's smoothstep(fadeRadius*0.8, fadeRadius, ...)) -- pass
    // the half-window itself (in probe-texel units) as fadeRadius.
    ubo.camParams[2] = (float)kProbeGridSize * 0.5f;
    ubo.camParams[3] = 0.0f;
    cb.PushFragmentUniforms(0, &ubo, sizeof(ubo));

    SDL_GPUViewport vp = { 0.f, 0.f, (float)sw, (float)sh, 0.f, 1.f };
    GpuSetViewport(pass, vp);
    cb.Draw(3);

    cb.EndPass();
}

void TerrainSkyVisAO::Shutdown() {
    if (!dev_) return;
    apply_pipeline_.Destroy();
    update_pipeline_.Destroy();
    if (probe_sampler_) { GpuReleaseSampler(dev_, probe_sampler_); probe_sampler_ = nullptr; }
    if (probe_tex_) { GpuReleaseTexture(dev_, probe_tex_); probe_tex_ = nullptr; }
    enabled_ = false;
    dev_ = nullptr;
}
#endif

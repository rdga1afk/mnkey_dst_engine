#include <monkey_dust/render/rd_pipeline.h>
#include <monkey_dust/render/rd_device.h>
#include <cstdio>

namespace md {

bool RdPipeline::Create(const Desc& d) {
    const RdDeviceCaps& caps = md::RdDevice::Get().Caps();

    GpuPipeline::Desc gpd;
    gpd.vert_path         = d.vert_path;
    gpd.frag_path         = d.frag_path;
    gpd.layout            = d.layout;
    gpd.raster            = d.raster;
    gpd.vert_uniform_bufs = d.vert_uniform_bufs;
    gpd.frag_uniform_bufs = d.frag_uniform_bufs;
    gpd.has_depth_target  = d.has_depth_target;
    gpd.depth_only        = d.depth_only;
    gpd.color_format      = d.color_format;

    gpd.vert_storage_bufs = d.vert_storage_bufs;
    if ((int)d.vert_storage_bufs > caps.max_vert_storage_bufs) {
        fprintf(stderr,
            "[RdPipeline] WARN '%s': vert_storage_bufs=%u > device limit %d — clamping\n",
            d.vert_path ? d.vert_path : "?",
            d.vert_storage_bufs, caps.max_vert_storage_bufs);
        gpd.vert_storage_bufs = (uint32_t)caps.max_vert_storage_bufs;
    }

    gpd.frag_samplers = d.frag_samplers;
    if ((int)d.frag_samplers > caps.max_frag_samplers) {
        fprintf(stderr,
            "[RdPipeline] WARN '%s': frag_samplers=%u > device limit %d — clamping\n",
            d.frag_path ? d.frag_path : "?",
            d.frag_samplers, caps.max_frag_samplers);
        gpd.frag_samplers = (uint32_t)caps.max_frag_samplers;
    }

    return gp_.Create(gpd);
}

void RdPipeline::Destroy() {
    gp_.Destroy();
}

} // namespace md

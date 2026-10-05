#ifdef MD_SDL_GPU
#include <monkey_dust/render/ambient_probe.h>
#include <monkey_dust/platform/md_log.h>
#include <cstring>
#include <cmath>

AmbientProbeSystem& AmbientProbeSystem::Get() {
    static AmbientProbeSystem inst;
    return inst;
}

void AmbientProbeSystem::Init() {
    count_ = 0;
    memset(probes_, 0, sizeof(probes_));
    // 8 KB SSBO — trivial even for HD 520 VRAM budget
    ssbo_.Init(static_cast<int>(MAX_PROBES * sizeof(AmbientProbe)));
    MD_LOG(MD_LOG_INFO, "AmbientProbeSystem: SSBO binding=%d, %d×128B = %d KB",
           SSBO_BINDING, MAX_PROBES,
           static_cast<int>(MAX_PROBES * sizeof(AmbientProbe) / 1024));
}

int AmbientProbeSystem::PlaceProbe(float x, float y, float z, float radius) {
    if (count_ >= MAX_PROBES) {
        MD_LOG(MD_LOG_WARNING, "AmbientProbeSystem: MAX_PROBES=%d reached", MAX_PROBES);
        return -1;
    }
    AmbientProbe& p = probes_[count_];
    memset(&p, 0, sizeof(p));
    p.pos_x  = x;
    p.pos_y  = y;
    p.pos_z  = z;
    p.radius = radius;
    return count_++;
}

void AmbientProbeSystem::SetAmbientColor(int idx, float r, float g, float b) {
    if (idx < 0 || idx >= count_) return;
    // L0 SH coefficient = color / (2 * sqrt(π)) so eval gives back the color
    // For practical game use: store raw color in sh[0]; shader multiplies by 1/(2√π).
    probes_[idx].sh_r[0] = r;
    probes_[idx].sh_g[0] = g;
    probes_[idx].sh_b[0] = b;
}

void AmbientProbeSystem::SetSkyLight(int idx,
                                     float dx, float dy, float dz,
                                     float r,  float g,  float b) {
    if (idx < 0 || idx >= count_) return;
    // Normalize direction
    float len = sqrtf(dx*dx + dy*dy + dz*dz);
    if (len < 1e-6f) return;
    float inv = 1.f / len;
    dx *= inv; dy *= inv; dz *= inv;

    // L1 SH bands: sh[1]=x, sh[2]=y, sh[3]=z contribution
    // Coefficient = color * direction_component * (√(3/(4π)) ≈ 0.4886)
    // Stored raw so shader reconstructs: irr += sh[1..3] * normal[xyz]
    constexpr float L1_SCALE = 0.4886f;
    probes_[idx].sh_r[1] += r * dx * L1_SCALE;
    probes_[idx].sh_r[2] += r * dy * L1_SCALE;
    probes_[idx].sh_r[3] += r * dz * L1_SCALE;
    probes_[idx].sh_g[1] += g * dx * L1_SCALE;
    probes_[idx].sh_g[2] += g * dy * L1_SCALE;
    probes_[idx].sh_g[3] += g * dz * L1_SCALE;
    probes_[idx].sh_b[1] += b * dx * L1_SCALE;
    probes_[idx].sh_b[2] += b * dy * L1_SCALE;
    probes_[idx].sh_b[3] += b * dz * L1_SCALE;
}

void AmbientProbeSystem::Upload() {
    if (count_ == 0) return;
    ssbo_.Upload(probes_, static_cast<int>(count_ * sizeof(AmbientProbe)));
}

void AmbientProbeSystem::UploadInCmd(md::GpuCommandBufferHandle cmd) {
    if (count_ == 0) return;
    ssbo_.UploadInCmd(cmd, probes_, static_cast<int>(count_ * sizeof(AmbientProbe)));
}

void AmbientProbeSystem::Clear() {
    count_ = 0;
    memset(probes_, 0, sizeof(probes_));
}

void AmbientProbeSystem::Shutdown() {
    ssbo_.Shutdown();
    count_ = 0;
}

#endif // MD_SDL_GPU

#pragma once
#include <cstdint>

#ifdef __SSE2__
#include <xmmintrin.h>
#include <emmintrin.h>
#endif

static constexpr int   MAX_PARTICLES               = 8192;
// VBfA-derived particle cull constants (RE 2026-06-25)
static constexpr float PARTICLE_VIS_PADDING          = 100.0f; // start rendering 100u before dist threshold
static constexpr float PARTICLE_VIS_DIST_DEFAULT     = 500.0f; // per-system max visibility distance

enum class ParticleType : uint8_t { SPARK = 0, SMOKE = 1 };

struct ParticleVertex {
    float   x, y, z, size;
    uint8_t r, g, b, a;
}; // stride = 20

class ParticleSoA {
public:
    static ParticleSoA& Get() {
        static ParticleSoA inst;
        return inst;
    }

    alignas(16) float px[MAX_PARTICLES];
    alignas(16) float py[MAX_PARTICLES];
    alignas(16) float pz[MAX_PARTICLES];
    alignas(16) float vx[MAX_PARTICLES];
    alignas(16) float vy[MAX_PARTICLES];
    alignas(16) float vz[MAX_PARTICLES];
    alignas(16) float life[MAX_PARTICLES];

    float   size[MAX_PARTICLES];
    uint8_t r[MAX_PARTICLES], g[MAX_PARTICLES], b[MAX_PARTICLES], a[MAX_PARTICLES];
    int     active_count = 0;

    void Update(float dt);

    // Camera-distance culled build (VBfA-OPT-1).
    // max_dist=0 → no culling. Pass PARTICLE_VIS_DIST_DEFAULT for normal use.
    int BuildVertices(ParticleVertex* out, int max_out,
                      float cam_x = 0.f, float cam_y = 0.f, float cam_z = 0.f,
                      float max_dist = 0.f) const;

private:
    ParticleSoA();
};

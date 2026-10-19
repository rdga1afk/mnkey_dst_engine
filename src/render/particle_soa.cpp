#include <monkey_dust/render/particle_soa.h>
#include <cstring>

ParticleSoA::ParticleSoA() {
    memset(life, 0, sizeof(life));
    active_count = 0;
}

void ParticleSoA::Update(float dt) {
#ifdef __SSE2__
    __m128 dt4 = _mm_set1_ps(dt);
    __m128 zero4 = _mm_setzero_ps();
    int n4 = active_count & ~3;
    for (int i = 0; i < n4; i += 4) {
        _mm_store_ps(px + i, _mm_add_ps(_mm_load_ps(px + i),
                                         _mm_mul_ps(_mm_load_ps(vx + i), dt4)));
        _mm_store_ps(py + i, _mm_add_ps(_mm_load_ps(py + i),
                                         _mm_mul_ps(_mm_load_ps(vy + i), dt4)));
        _mm_store_ps(pz + i, _mm_add_ps(_mm_load_ps(pz + i),
                                         _mm_mul_ps(_mm_load_ps(vz + i), dt4)));
        _mm_store_ps(life + i, _mm_sub_ps(_mm_load_ps(life + i), dt4));
        (void)zero4;
    }
    for (int i = n4; i < active_count; ++i) {
        px[i] += vx[i] * dt;
        py[i] += vy[i] * dt;
        pz[i] += vz[i] * dt;
        life[i] -= dt;
    }
#else
    for (int i = 0; i < active_count; ++i) {
        px[i] += vx[i] * dt;
        py[i] += vy[i] * dt;
        pz[i] += vz[i] * dt;
        life[i] -= dt;
    }
#endif

    // compact dead particles (swap-with-last)
    for (int i = active_count - 1; i >= 0; --i) {
        if (life[i] <= 0.0f) {
            int last = --active_count;
            if (i != last) {
                px[i] = px[last]; py[i] = py[last]; pz[i] = pz[last];
                vx[i] = vx[last]; vy[i] = vy[last]; vz[i] = vz[last];
                life[i] = life[last]; size[i] = size[last];
                r[i] = r[last]; g[i] = g[last]; b[i] = b[last]; a[i] = a[last];
            }
        }
    }
}

int ParticleSoA::BuildVertices(ParticleVertex* out, int max_out,
                                float cam_x, float cam_y, float cam_z,
                                float max_dist) const {
    const float max_dist_sq = max_dist > 0.0f ? max_dist * max_dist : 0.0f;
    int count = 0;
    for (int i = 0; i < active_count && count < max_out; ++i) {
        if (life[i] <= 0.0f) continue;
        if (max_dist_sq > 0.0f) {
            float dx = px[i] - cam_x, dy = py[i] - cam_y, dz = pz[i] - cam_z;
            if (dx*dx + dy*dy + dz*dz > max_dist_sq) continue;
        }
        ParticleVertex& v = out[count++];
        v.x = px[i]; v.y = py[i]; v.z = pz[i];
        v.size = size[i];
        v.r = r[i]; v.g = g[i]; v.b = b[i]; v.a = a[i];
    }
    return count;
}


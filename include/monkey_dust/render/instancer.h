#pragma once
#include <monkey_dust/platform/math_types.h>


static constexpr int MAX_INSTANCES = 1024;

// Batch static objects via GL instanced draw.
// One draw call for hundreds of identical meshes (trees, rocks, ruins).
// Usage:
//   Instancer trees;
//   trees.Add(x, y, z, scale);                   // at chunk load
//   glUseProgram(shader.id);
//   trees.Draw(mesh, loc_vp, mat4_ptr(vp));       // each frame
//   glUseProgram(0);
//   trees.Shutdown();                             // at cleanup

class Instancer {
public:
    void Reset() { count_ = 0; }

    bool Add(float x, float y, float z, float scale = 1.0f) {
        if (count_ >= MAX_INSTANCES) return false;
        transforms_[count_] = mat4_mul(
            mat4_scale(scale, scale, scale),
            mat4_translate(x, y, z)
        );
        count_++;
        return true;
    }


    int          Count()      const { return count_; }
    const Mat4*  Transforms() const { return transforms_; }

private:
    Mat4         transforms_[MAX_INSTANCES];
    int          count_    = 0;
};

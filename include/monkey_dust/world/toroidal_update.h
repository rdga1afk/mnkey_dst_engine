#pragma once
// ToroidalUpdate — reusable primitive for wrap-addressed caches (terrain
// detail atlases, shadow/GI clipmaps, any physically-fixed-size buffer that
// follows the camera). Splits a camera-origin shift into up to 4 axis-
// aligned quad regions that actually need re-rendering; the rest of the
// physical buffer stays valid, addressed via wrap (toroidal) offset instead
// of being re-copied.
//
// docs/DAGOR_IMPLEMENTATION_PROMPT.md Рівень 2, КРОК 4 -- native
// reimplementation of the algorithm documented in
// docs/DAGOR_ANALYSIS_2026-09.md (Dagor's render/toroidal_update.h), not a
// line-for-line port: own types (IPoint2 below, not dag::/IPoint2 from
// Dagor's math lib), own error-handling convention (MD_LOG_WARNING, not
// G_ASSERT), no external container dependency (fixed ToroidalQuadRegion[4]
// out-array, not dag::ConstSpan/StaticTab). The core algorithm (quadrant
// splitting via old/new-center clamping) is the same well-known technique,
// verified independently against a battery of cases in
// toroidal_update_test.cpp.
//
// General-purpose, NOT terrain-specific -- usable for any future toroidal
// cache (terrain detail atlas redesign, KROK 5; shadow/GI clipmaps).

#include <algorithm>
#include <cstdlib>

namespace md {

struct IPoint2 {
    int x = 0, y = 0;
    IPoint2() = default;
    IPoint2(int x_, int y_) : x(x_), y(y_) {}
    IPoint2 operator+(IPoint2 o) const { return {x + o.x, y + o.y}; }
    IPoint2 operator-(IPoint2 o) const { return {x - o.x, y - o.y}; }
    bool operator==(IPoint2 o) const { return x == o.x && y == o.y; }
};

inline IPoint2 IPMin(IPoint2 a, IPoint2 b) { return {std::min(a.x, b.x), std::min(a.y, b.y)}; }
inline IPoint2 IPMax(IPoint2 a, IPoint2 b) { return {std::max(a.x, b.x), std::max(a.y, b.y)}; }
inline IPoint2 IPAbs(IPoint2 a) { return {std::abs(a.x), std::abs(a.y)}; }

// Wraps a signed texel coordinate into [0, tex_size) on both axes.
inline IPoint2 ToroidalWrap(IPoint2 p, int tex_size) {
    int wx = p.x % tex_size;
    if (wx < 0) { wx += tex_size; }
    int wy = p.y % tex_size;
    if (wy < 0) { wy += tex_size; }
    return {wx, wy};
}

// One region that needs re-rendering: write into the physical buffer at
// [viewport_lt, viewport_lt+size), sourcing/computing content for the
// corresponding world-texel-space origin world_texel_from (same convention
// as SampleWorld-style lookups elsewhere in this codebase -- add the
// region's local (u,v) offset to world_texel_from to get the absolute
// world-texel coordinate for texel (u,v) of this region).
struct ToroidalQuadRegion {
    IPoint2 viewport_lt;
    IPoint2 size;
    IPoint2 world_texel_from;
};

namespace detail {
inline int EmitQuad(IPoint2 lt, IPoint2 wd, IPoint2 new_center, IPoint2 main_center,
                     int tex_size, ToroidalQuadRegion* out, int& out_count) {
    IPoint2 clampedWd = IPMin(wd, IPoint2(tex_size, tex_size) - lt);
    if (clampedWd.x <= 0 || clampedWd.y <= 0) { return 0; }
    IPoint2 newLT = new_center - IPoint2(tex_size / 2, tex_size / 2);
    IPoint2 shift = ToroidalWrap(main_center - new_center, tex_size);
    IPoint2 texelsFrom = ToroidalWrap(lt + shift, tex_size) + newLT;
    out[out_count++] = ToroidalQuadRegion{lt, clampedWd, texelsFrom};
    return clampedWd.x * clampedWd.y;
}
} // namespace detail

// Computes which regions of a `tex_size`x`tex_size` toroidal cache need
// re-rendering when the tracked origin moves from `cur_origin` to
// `new_origin` (both in world-texel units). `main_origin` is the cache's
// fixed reference point (texel (0,0) of the physical buffer corresponds to
// main_origin - tex_size/2 in world-texel space) -- pass the same
// `main_origin` every call; it only changes (to new_origin) on a full
// invalidation.
//
// Writes up to 4 regions into `out_regions` (caller-provided, size 4) and
// returns how many were written (0 = no movement, nothing to do).
// `*cur_origin` is updated in place to the post-update origin (matches
// `new_origin` after a normal partial update, or is reset alongside
// `main_origin` on a full invalidation -- caller must also update its own
// main_origin to the returned value via the out_new_main_origin pointer).
//
// full_update_texels_threshold: if the movement is this large or more (or
// >= tex_size), everything is invalidated as ONE full-size region instead
// of computing 4 partial quads.
inline int ToroidalUpdate(IPoint2 new_origin, IPoint2* cur_origin, IPoint2* main_origin,
                           int tex_size, int full_update_texels_threshold,
                           ToroidalQuadRegion out_regions[4]) {
    IPoint2 movement = *cur_origin - new_origin;
    IPoint2 updateBox = IPMin(IPAbs(movement), IPoint2(tex_size, tex_size));
    int maxMovement = std::max(updateBox.x, updateBox.y);
    if (maxMovement == 0) { return 0; }

    if (maxMovement >= std::min(tex_size, full_update_texels_threshold)) {
        *cur_origin = new_origin;
        *main_origin = new_origin;
        out_regions[0] = ToroidalQuadRegion{
            IPoint2(0, 0), IPoint2(tex_size, tex_size),
            IPoint2(new_origin.x - (tex_size / 2), new_origin.y - (tex_size / 2))};
        return 1;
    }

    IPoint2 oldCenter = ToroidalWrap(*cur_origin - *main_origin, tex_size);
    IPoint2 newCenter = oldCenter + (new_origin - *cur_origin);

    // Clamp each axis into [0, tex_size], same edge-case handling as the
    // reference algorithm: if the naive shift would push the center
    // negative/past the far edge, either snap to the boundary (if the old
    // center was already past it, meaning we're sliding further off) or
    // wrap fully around (if the old center was still within bounds).
    auto clampAxis = [tex_size](int oldC, int& newC) {
        if (newC < 0) {
            if (oldC > 0) { newC = 0; }
            else { newC += tex_size; }
        } else if (newC > tex_size) {
            newC = tex_size;
        }
    };
    IPoint2 oldCenterAdj = oldCenter;
    if (newCenter.x < 0 && !(oldCenter.x > 0)) { oldCenterAdj.x = tex_size; }
    if (newCenter.y < 0 && !(oldCenter.y > 0)) { oldCenterAdj.y = tex_size; }
    clampAxis(oldCenter.x, newCenter.x);
    clampAxis(oldCenter.y, newCenter.y);
    oldCenter = oldCenterAdj;

    IPoint2 origin = *cur_origin + (newCenter - oldCenter);
    IPoint2 minC = IPMin(newCenter, oldCenter);
    IPoint2 maxC = IPMax(newCenter, oldCenter);
    IPoint2 viewportLeft(tex_size - maxC.x, tex_size - maxC.y);

    int count = 0;
    detail::EmitQuad(IPoint2(0, minC.y), IPoint2(newCenter.x, updateBox.y), origin, *main_origin, tex_size, out_regions, count);
    detail::EmitQuad(IPoint2(minC.x, 0), IPoint2(updateBox.x, minC.y), origin, *main_origin, tex_size, out_regions, count);
    detail::EmitQuad(IPoint2(newCenter.x, minC.y), IPoint2(tex_size - newCenter.x, updateBox.y), origin, *main_origin, tex_size, out_regions, count);
    detail::EmitQuad(IPoint2(minC.x, maxC.y), IPoint2(updateBox.x, viewportLeft.y), origin, *main_origin, tex_size, out_regions, count);

    *cur_origin = origin;
    return count;
}

} // namespace md

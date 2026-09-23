#pragma once

#include <fuse/math/vec.hpp>
#include <fuse/types.hpp>

#include <algorithm>
#include <cmath>

namespace fuse::renderer {

/// SDF soft shadows (B5.5). Device-safe (FUSE_HOST_DEVICE): the full-frame `sdf_shadows` kernel
/// (shadow/sdf_shadow_kernel.hpp) runs this march against the ray-march scene SDF on every backend.
///
/// Penumbra ray marching along the shadow ray with the improved distance estimate: consecutive SDF
/// samples h_prev, h bound the closest approach between them at y = h^2 / (2 h_prev) behind the
/// current sample with clearance d = sqrt(h^2 - y^2), so the angular clearance is estimated as
/// d / (t - y) instead of only at the (coarse) sample points. The estimate is only used when that
/// point lies between the two samples (otherwise, e.g. when leaving a surface, it would collapse to 0). The ray keeps marching through the occluder with the signed
/// distance so the umbra side of the penumbra is resolved too (clearance < 0).
///
/// Light model: `penumbraK` corresponds to a spherical (disc-shaped) light of angular radius
/// theta = 1 / penumbraK (small-angle; see `sdfPenumbraLightAngularRadius`). The normalised clearance
/// r = penumbraK * clearance in [-1, 1] is the signed offset of the occluder silhouette from the light
/// centre in units of theta; the returned factor is the exact fraction of a uniform disc left visible
/// by a straight silhouette edge at that offset (`sdfDiscVisibility`). Higher k = sharper shadow.

struct SdfSoftShadowParams {
    f32 tMin = 0.01f;
    f32 tMax = 100.f;
    f32 penumbraK = 16.f;
    u32 maxSteps = 64u;
    /// Minimum march step (progress guarantee when grazing / inside an occluder).
    f32 minStep = 0.002f;
    /// March step as a fraction of |h|. 1 = plain sphere tracing (fastest). The improved estimate is a
    /// lower bound on the clearance between samples; next to flat faces parallel to the ray it is
    /// pessimistic by 1 - sqrt(1 - s^2/4) (13% at s = 1, 0.8% at s = 0.25), widening the lit side of
    /// the penumbra accordingly — smaller steps trade march cost for accuracy.
    f32 stepScale = 1.f;
    /// Surface epsilon of the scene SDF.
    f32 hitEpsilon = 1e-4f;
};

/// Angular radius (radians) of the spherical light equivalent to `penumbraK`.
FUSE_HOST_DEVICE inline f32 sdfPenumbraLightAngularRadius(f32 penumbraK) {
    return penumbraK > 0.f ? 1.f / penumbraK : 0.f;
}

/// Fraction of a uniform disc on the visible side of a straight edge at signed offset `r` from the disc
/// centre (r in units of the disc radius; r = -1 fully covered, r = +1 fully visible).
FUSE_HOST_DEVICE inline f32 sdfDiscVisibility(f32 r) {
    const f32 c = std::clamp(r, -1.f, 1.f);
    constexpr f32 kInvPi = 0.318309886f;
    return 0.5f + kInvPi * (c * std::sqrt(std::max(0.f, 1.f - c * c)) + std::asin(c));
}

/// Signed normalised clearance r = k * min_t(angular clearance) in [-1, 1] (1 = light fully clear).
template <typename SceneSdf>
FUSE_HOST_DEVICE f32 sdfSoftShadowClearance(const SceneSdf& sceneSdf,
                           const fuse::math::Vec3& rayOrigin,
                           const fuse::math::Vec3& rayDir,
                           const SdfSoftShadowParams& params) {
    f32 res = 1.f;
    f32 t = params.tMin;
    f32 prev = 1e30f;
    f32 step = 0.f;

    for (u32 i = 0; i < params.maxSteps && t < params.tMax; ++i) {
        const fuse::math::Vec3 p = rayOrigin + rayDir * t;
        const f32 h = sceneSdf(p);

        f32 clearance = h / t;
        if (h > params.hitEpsilon && prev > params.hitEpsilon && prev < 1e29f && step > 0.f) {
            // Improved penumbra estimate: the unbounding spheres of radius `prev` and `h` around the
            // previous / current sample intersect on a circle at distance y behind the current sample
            // (y = h^2 / (2 prev) for a full sphere-tracing step). The closest the surface can come to
            // the ray between the two samples is that circle's radius d = sqrt(h^2 - y^2).
            const f32 y = (h * h - prev * prev + step * step) / (2.f * step);
            if (y > 0.f && y < step && y < h) {
                const f32 d = std::sqrt(h * h - y * y);
                clearance = std::min(clearance, d / std::max(1e-6f, t - y));
            }
        }

        res = std::min(res, params.penumbraK * clearance);
        if (res <= -1.f) {
            return -1.f;
        }

        prev = h;
        step = std::max(std::fabs(h) * params.stepScale, params.minStep);
        t += step;
    }

    return std::clamp(res, -1.f, 1.f);
}

/// Shadow factor in [0, 1]: 1 = fully lit, 0 = fully shadowed.
template <typename SceneSdf>
FUSE_HOST_DEVICE f32 sdfSoftShadow(const SceneSdf& sceneSdf,
                  const fuse::math::Vec3& rayOrigin,
                  const fuse::math::Vec3& rayDir,
                  f32 tMin,
                  f32 tMax,
                  f32 penumbraK,
                  u32 maxSteps = 64u) {
    SdfSoftShadowParams params{};
    params.tMin = tMin;
    params.tMax = tMax;
    params.penumbraK = penumbraK;
    params.maxSteps = maxSteps;
    return sdfDiscVisibility(sdfSoftShadowClearance(sceneSdf, rayOrigin, rayDir, params));
}

/// Shadow factor in [0, 1] with full march parameters.
template <typename SceneSdf>
FUSE_HOST_DEVICE f32 sdfSoftShadow(const SceneSdf& sceneSdf,
                  const fuse::math::Vec3& rayOrigin,
                  const fuse::math::Vec3& rayDir,
                  const SdfSoftShadowParams& params) {
    return sdfDiscVisibility(sdfSoftShadowClearance(sceneSdf, rayOrigin, rayDir, params));
}

} // namespace fuse::renderer

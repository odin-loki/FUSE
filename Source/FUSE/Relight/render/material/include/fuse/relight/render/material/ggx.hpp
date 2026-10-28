// Isotropic GGX / Trowbridge-Reitz normal distribution (CPU). Not the BSDF shader path.
//
// D = alpha^2 / (pi * denom^2), denom = nDotH^2 * (alpha^2 - 1) + 1.
// nDotH is clamped to 0..1. A non-finite nDotH, or an alpha that is <= 0 or
// non-finite, returns 0. alpha is then clamped to [1e-3, 1] so D stays finite.
#pragma once

#include <algorithm>
#include <cmath>
#include <numbers>

namespace fuse::relight::render::material {

/// Trowbridge-Reitz NDF D(n·h). See the file comment for the clamps.
[[nodiscard]] inline float ggx_ndf(float nDotH, float alpha) {
    if (!std::isfinite(nDotH) || !std::isfinite(alpha) || alpha <= 0.f) {
        return 0.f;
    }
    const float nh = std::clamp(nDotH, 0.f, 1.f);
    const float a = std::clamp(alpha, 1e-3f, 1.f);
    const float a2 = a * a;
    const float denom = nh * nh * (a2 - 1.f) + 1.f;
    constexpr float kPi = std::numbers::pi_v<float>;
    return a2 / (kPi * denom * denom);
}

} // namespace fuse::relight::render::material

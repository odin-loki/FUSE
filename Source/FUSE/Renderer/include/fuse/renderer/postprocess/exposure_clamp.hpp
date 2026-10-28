#pragma once

#include <cmath>

namespace fuse::renderer {

/// Orders the bounds, then clamps a linear exposure multiplier. Separate from EV conversion.
/// A non-finite exposure returns the finite lower bound, or 1 when that bound is non-finite.
/// A non-positive exposure returns the lower bound. Every other value clamps into [min, max].
[[nodiscard]] inline float clamp_exposure(float exposure, float minExposure, float maxExposure) {
    if (minExposure > maxExposure) {
        const float lower = maxExposure;
        maxExposure = minExposure;
        minExposure = lower;
    }
    if (!std::isfinite(exposure)) {
        return std::isfinite(minExposure) ? minExposure : 1.f;
    }
    if (exposure <= 0.f) {
        return minExposure;
    }
    if (exposure < minExposure) {
        return minExposure;
    }
    if (exposure > maxExposure) {
        return maxExposure;
    }
    return exposure;
}

} // namespace fuse::renderer

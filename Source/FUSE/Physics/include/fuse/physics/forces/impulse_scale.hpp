#pragma once

#include <cmath>

namespace fuse::physics {

/// Scales an impulse. Non-finite inputs return 0. A negative impulse counts as 0.
/// Scale is clamped to 0..4. A non-finite product returns 0.
[[nodiscard]] inline float scale_impulse(float impulse, float scale) {
    if (!std::isfinite(impulse) || !std::isfinite(scale)) {
        return 0.f;
    }
    if (impulse < 0.f) {
        impulse = 0.f;
    }
    if (scale < 0.f) {
        scale = 0.f;
    } else if (scale > 4.f) {
        scale = 4.f;
    }
    const float scaled = impulse * scale;
    if (!std::isfinite(scaled)) {
        return 0.f;
    }
    return scaled;
}

} // namespace fuse::physics

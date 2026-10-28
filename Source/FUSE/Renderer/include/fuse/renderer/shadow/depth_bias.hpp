#pragma once

#include <algorithm>
#include <cmath>

namespace fuse::renderer {

/// Slope-scaled shadow depth bias: slopeScale * 1e-4 + constant.
/// Non-finite inputs return 0. Negative inputs count as 0.
/// slopeScale is clamped to [0, 8] and constant to [0, 0.01].
[[nodiscard]] inline float shadow_depth_bias(float slopeScale, float constant) {
    if (!std::isfinite(slopeScale) || !std::isfinite(constant)) {
        return 0.f;
    }
    if (slopeScale < 0.f) {
        slopeScale = 0.f;
    }
    if (constant < 0.f) {
        constant = 0.f;
    }
    slopeScale = std::clamp(slopeScale, 0.f, 8.f);
    constant = std::clamp(constant, 0.f, 0.01f);
    return slopeScale * 1e-4f + constant;
}

} // namespace fuse::renderer

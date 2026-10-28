#pragma once

#include <cmath>

namespace fuse::math {

/// Clamp `value` to [0, 1]. A non-finite input returns 0.
[[nodiscard]] inline float saturate01(float value) {
    if (!std::isfinite(value) || value < 0.f) {
        return 0.f;
    }
    if (value > 1.f) {
        return 1.f;
    }
    return value;
}

} // namespace fuse::math

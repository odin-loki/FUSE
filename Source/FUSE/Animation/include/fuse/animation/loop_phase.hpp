#pragma once

#include <cmath>

namespace fuse::animation {

/// Normalized loop phase of `time` over `duration`, in [0, 1).
/// Non-finite inputs and `duration <= 0` return 0.
[[nodiscard]] inline float loop_phase(float time, float duration) {
    if (!std::isfinite(time) || !std::isfinite(duration) || !(duration > 0.f)) {
        return 0.f;
    }

    float remainder = std::fmod(time, duration);
    if (remainder < 0.f) {
        remainder += duration;
    }
    return remainder / duration;
}

} // namespace fuse::animation

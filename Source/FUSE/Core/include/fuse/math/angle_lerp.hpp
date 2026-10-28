#pragma once

#include <cmath>

namespace fuse::math {

/// Short-arc lerp in degrees. `to - from` is wrapped into (-180, 180], `t` is clamped
/// to [0, 1], and the result is wrapped into [0, 360). Non-finite inputs return 0.
[[nodiscard]] inline float lerp_angle_deg(float from, float to, float t) {
    if (!std::isfinite(from) || !std::isfinite(to) || !std::isfinite(t)) {
        return 0.f;
    }
    if (t < 0.f) {
        t = 0.f;
    } else if (t > 1.f) {
        t = 1.f;
    }

    float delta = std::fmod(to - from, 360.f);
    if (!std::isfinite(delta)) {
        return 0.f;
    }
    if (delta < 0.f) {
        delta += 360.f;
    }
    if (delta > 180.f) {
        delta -= 360.f;
    }

    float result = std::fmod(from + delta * t, 360.f);
    if (!std::isfinite(result)) {
        return 0.f;
    }
    if (result < 0.f) {
        result += 360.f;
    }
    return result;
}

} // namespace fuse::math

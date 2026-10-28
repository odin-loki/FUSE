#pragma once

#include <cmath>

namespace fuse::adventure {

/// Yaw in degrees wrapped into [0, 360). A non-finite input returns 0.
[[nodiscard]] inline float wrap_spawn_yaw(float degrees) {
    if (!std::isfinite(degrees)) {
        return 0.f;
    }
    float wrapped = std::fmod(degrees, 360.f);
    if (!std::isfinite(wrapped)) {
        return 0.f;
    }
    if (wrapped < 0.f) {
        wrapped += 360.f;
    }
    if (!(wrapped < 360.f)) {
        return 0.f;
    }
    return wrapped;
}

} // namespace fuse::adventure

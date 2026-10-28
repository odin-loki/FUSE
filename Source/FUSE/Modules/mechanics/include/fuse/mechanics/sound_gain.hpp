#pragma once

#include <algorithm>
#include <cmath>

namespace fuse::mechanics {

/// Clamp a linear sound gain to [0, 1]. A non-finite gain returns 0.
[[nodiscard]] inline float clamp_sound_gain(float gain) {
    if (!std::isfinite(gain)) {
        return 0.f;
    }
    return std::clamp(gain, 0.f, 1.f);
}

} // namespace fuse::mechanics

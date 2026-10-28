#pragma once

#include <cmath>

namespace fuse::terrain {

/// Angle in radians from vertical rise over horizontal run.
/// Non-finite inputs return 0. A zero rise with a zero run returns 0.
[[nodiscard]] inline float slope_radians(float rise, float run) {
    if (!std::isfinite(rise) || !std::isfinite(run)) {
        return 0.f;
    }
    if (rise == 0.f && run == 0.f) {
        return 0.f;
    }
    return std::atan2(rise, run);
}

} // namespace fuse::terrain

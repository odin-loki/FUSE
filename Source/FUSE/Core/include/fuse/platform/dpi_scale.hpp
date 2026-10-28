#pragma once

#include <cmath>

namespace fuse::platform {

/// Clamp a DPI scale to [1, 4]. A non-finite value, or any scale below 1, returns 1.
/// A scale above 4 returns 4. Every other scale is returned unchanged.
[[nodiscard]] inline float snap_dpi_scale(float scale) {
    if (!std::isfinite(scale) || scale < 1.f) {
        return 1.f;
    }
    if (scale > 4.f) {
        return 4.f;
    }
    return scale;
}

} // namespace fuse::platform

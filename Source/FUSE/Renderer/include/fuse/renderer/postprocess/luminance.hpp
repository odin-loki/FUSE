#pragma once

#include <cmath>

namespace fuse::renderer {

/// Rec. 709 relative luminance: 0.2126*r + 0.7152*g + 0.0722*b.
/// Non-finite channels count as 0. Negative channels count as 0.
[[nodiscard]] inline float relative_luminance(float r, float g, float b) {
    if (!std::isfinite(r) || r < 0.f) {
        r = 0.f;
    }
    if (!std::isfinite(g) || g < 0.f) {
        g = 0.f;
    }
    if (!std::isfinite(b) || b < 0.f) {
        b = 0.f;
    }
    return 0.2126f * r + 0.7152f * g + 0.0722f * b;
}

} // namespace fuse::renderer

#pragma once

#include <cmath>

namespace fuse::mechanics {

/// Inclusive 2D circle test. Non-finite inputs and a non-positive radius are outside.
[[nodiscard]] inline bool circle_contains(float px, float py, float cx, float cy, float radius) {
    if (!std::isfinite(px) || !std::isfinite(py) || !std::isfinite(cx) || !std::isfinite(cy) ||
        !std::isfinite(radius)) {
        return false;
    }
    if (radius <= 0.f) {
        return false;
    }
    const float dx = px - cx;
    const float dy = py - cy;
    return (dx * dx + dy * dy) <= (radius * radius);
}

} // namespace fuse::mechanics

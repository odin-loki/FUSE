#pragma once

#include <cmath>

namespace fuse::editor {

/// Play-mode delta time. A paused clock yields 0. Non-positive and non-finite
/// deltas yield 0. A non-finite scale yields 0. Otherwise scale is clamped to
/// [0, 4] and the result is dt * scale.
[[nodiscard]] inline float scaled_dt(float dt, float scale, bool paused) {
    if (paused) {
        return 0.f;
    }
    if (!(dt > 0.f) || !std::isfinite(dt)) {
        return 0.f;
    }
    if (!std::isfinite(scale)) {
        return 0.f;
    }
    if (scale < 0.f) {
        scale = 0.f;
    } else if (scale > 4.f) {
        scale = 4.f;
    }
    return dt * scale;
}

} // namespace fuse::editor

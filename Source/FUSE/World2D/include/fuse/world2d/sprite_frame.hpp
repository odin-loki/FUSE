#pragma once

#include <cmath>

namespace fuse::world2d {

/// Index of the sprite frame playing at `time`.
///
/// `frameCount <= 0`, `fps <= 0`, or any non-finite input returns 0. Otherwise
/// `floor(time * fps)` is wrapped into [0, frameCount). Negative time wraps to a
/// non-negative index.
[[nodiscard]] inline int sprite_frame_index(float time, float fps, int frameCount) {
    if (frameCount <= 0 || !std::isfinite(time) || !std::isfinite(fps) || !(fps > 0.f)) {
        return 0;
    }

    const float scaled = time * fps;
    if (!std::isfinite(scaled)) {
        return 0;
    }

    const double steps = std::floor(static_cast<double>(scaled));
    const double span = static_cast<double>(frameCount);
    double wrapped = std::fmod(steps, span);
    if (wrapped < 0.0) {
        wrapped += span;
    }
    if (!std::isfinite(wrapped) || wrapped < 0.0 || wrapped >= span) {
        return 0;
    }
    return static_cast<int>(wrapped);
}

} // namespace fuse::world2d

#pragma once

#include <fuse/types.hpp>

#include <cmath>

namespace fuse::animation {

/// Wraps `time` into [0, duration). Negative time wraps upward.
/// Non-finite inputs and `duration <= 0` return 0.
[[nodiscard]] inline f32 wrap_clip_time(f32 time, f32 duration) {
    if (!std::isfinite(time) || !std::isfinite(duration) || !(duration > 0.f)) {
        return 0.f;
    }

    f32 wrapped = std::fmod(time, duration);
    if (wrapped < 0.f) {
        wrapped += duration;
    }
    if (!std::isfinite(wrapped) || wrapped < 0.f || wrapped >= duration) {
        return 0.f;
    }
    return wrapped;
}

} // namespace fuse::animation

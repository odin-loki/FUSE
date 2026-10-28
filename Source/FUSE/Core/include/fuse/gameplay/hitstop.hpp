#pragma once

#include <fuse/types.hpp>

#include <cmath>

namespace fuse::gameplay {

/// Freeze window. While `remaining` is positive, `scale_dt` yields 0 and drains the window.
struct Hitstop {
    f32 remaining = 0.f;
};

/// Extends the freeze to max(remaining, seconds). Non-finite or non-positive `seconds` is ignored.
inline void trigger(Hitstop& hitstop, f32 seconds) {
    if (!std::isfinite(seconds) || seconds <= 0.f) {
        return;
    }
    if (seconds > hitstop.remaining) {
        hitstop.remaining = seconds;
    }
}

/// Returns 0 while `remaining` > 0 and subtracts `dt` from it, clamped at 0.
/// Returns `dt` unchanged when `remaining` is 0. `dt <= 0` (and non-finite `dt`) returns 0
/// and leaves `remaining` unchanged.
[[nodiscard]] inline f32 scale_dt(Hitstop& hitstop, f32 dt) {
    if (!std::isfinite(dt) || dt <= 0.f) {
        return 0.f;
    }
    if (hitstop.remaining > 0.f) {
        hitstop.remaining -= dt;
        if (hitstop.remaining < 0.f) {
            hitstop.remaining = 0.f;
        }
        return 0.f;
    }
    return dt;
}

} // namespace fuse::gameplay

// FUSE Relight: clamp a frame delta into a finite range before a frame step uses it.
#pragma once

#include <cmath>

namespace fuse::relight::render::frame {

/// Clamp `dt` into [`minDt`, `maxDt`].
///
/// Bounds are ordered first: if `minDt` > `maxDt` they are swapped. A non-finite `dt` returns that
/// ordered minimum when it is finite, otherwise 0. A finite `dt` is clamped into the ordered range.
[[nodiscard]] inline float clamp_frame_dt(float dt, float minDt, float maxDt) {
    if (minDt > maxDt) {
        const float orderedMin = maxDt;
        maxDt = minDt;
        minDt = orderedMin;
    }
    if (!std::isfinite(dt)) {
        return std::isfinite(minDt) ? minDt : 0.f;
    }
    if (dt < minDt) {
        return minDt;
    }
    if (maxDt < dt) {
        return maxDt;
    }
    return dt;
}

} // namespace fuse::relight::render::frame

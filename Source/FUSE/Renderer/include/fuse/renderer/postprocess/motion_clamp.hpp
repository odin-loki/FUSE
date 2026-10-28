#pragma once

#include <fuse/math/vec.hpp>

#include <cmath>

namespace fuse::renderer {

/// Scale a screen-space motion vector down to `maxPixels` when it is longer.
/// A non-positive or non-finite limit, or a non-finite velocity, yields zero.
[[nodiscard]] inline math::Vec2 clamp_motion(math::Vec2 velocityPx, f32 maxPixels) {
    if (!std::isfinite(maxPixels) || maxPixels <= 0.f) {
        return {};
    }
    if (!std::isfinite(velocityPx.x) || !std::isfinite(velocityPx.y)) {
        return {};
    }
    const f32 len = velocityPx.length();
    if (!std::isfinite(len)) {
        return {};
    }
    if (len > maxPixels) {
        return velocityPx * (maxPixels / len);
    }
    return velocityPx;
}

} // namespace fuse::renderer

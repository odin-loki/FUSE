#pragma once

#include <fuse/math/vec.hpp>

#include <cmath>
#include <cstddef>
#include <span>

namespace fuse::navigation {

/// Euclidean length of a waypoint polyline in world xy.
/// An empty span or a single point is 0. A non-finite segment contributes 0.
[[nodiscard]] inline float path_length(std::span<const fuse::math::Vec2> points) {
    float total = 0.f;
    for (std::size_t i = 1; i < points.size(); ++i) {
        const float segment = (points[i] - points[i - 1]).length();
        if (std::isfinite(segment)) {
            total += segment;
        }
    }
    return total;
}

} // namespace fuse::navigation

#pragma once

#include <cmath>
#include <utility>

namespace fuse::ecs {

/// Axis-aligned box volume. Any non-finite component yields 0. An inverted axis is swapped.
[[nodiscard]] inline float aabb_volume(float minX, float minY, float minZ, float maxX, float maxY,
                                       float maxZ) {
    if (!std::isfinite(minX) || !std::isfinite(minY) || !std::isfinite(minZ) || !std::isfinite(maxX) ||
        !std::isfinite(maxY) || !std::isfinite(maxZ)) {
        return 0.f;
    }
    if (minX > maxX) {
        std::swap(minX, maxX);
    }
    if (minY > maxY) {
        std::swap(minY, maxY);
    }
    if (minZ > maxZ) {
        std::swap(minZ, maxZ);
    }
    const float dx = maxX - minX;
    const float dy = maxY - minY;
    const float dz = maxZ - minZ;
    return dx * dy * dz;
}

} // namespace fuse::ecs

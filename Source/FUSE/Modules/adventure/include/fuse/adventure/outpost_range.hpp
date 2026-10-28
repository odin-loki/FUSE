#pragma once

#include <cmath>

namespace fuse::adventure {

/// XZ distance from (x, z) to (originX, originZ). Non-finite inputs return -1.
[[nodiscard]] inline float outpost_distance(float x, float z, float originX, float originZ) {
    if (!std::isfinite(x) || !std::isfinite(z) || !std::isfinite(originX) || !std::isfinite(originZ)) {
        return -1.f;
    }
    const float dx = x - originX;
    const float dz = z - originZ;
    const float distance = std::sqrt(dx * dx + dz * dz);
    if (!std::isfinite(distance)) {
        return -1.f;
    }
    return distance;
}

/// True when the XZ distance from (x, z) to (originX, originZ) is <= radius.
/// A negative radius is outside. Any non-finite input is outside.
[[nodiscard]] inline bool outpost_in_range(float x, float z, float originX, float originZ, float radius) {
    if (!std::isfinite(radius) || radius < 0.f) {
        return false;
    }
    const float distance = outpost_distance(x, z, originX, originZ);
    if (distance < 0.f) {
        return false;
    }
    return distance <= radius;
}

} // namespace fuse::adventure

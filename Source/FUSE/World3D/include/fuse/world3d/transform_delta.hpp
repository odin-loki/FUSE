#pragma once

#include <cmath>

namespace fuse::world3d {

/// True when a translation delta should invalidate a cached transform.
/// Squared length `dx*dx + dy*dy + dz*dz` must be greater than `epsilon*epsilon`.
/// A negative epsilon is treated as 0. A non-finite dx, dy, or dz is dirty.
[[nodiscard]] inline bool transform_delta_dirty(float dx, float dy, float dz, float epsilon) {
    if (!std::isfinite(dx) || !std::isfinite(dy) || !std::isfinite(dz)) {
        return true;
    }
    if (epsilon < 0.f) {
        epsilon = 0.f;
    }
    const float lengthSquared = dx * dx + dy * dy + dz * dz;
    const float epsilonSquared = epsilon * epsilon;
    return lengthSquared > epsilonSquared;
}

} // namespace fuse::world3d

#pragma once

#include <fuse/physics/types.hpp>

#include <cmath>

namespace fuse::physics {

/// Sleep only when both speeds are at or below their thresholds.
/// A negative or non-finite threshold is treated as 0. A non-finite speed stays awake.
[[nodiscard]] inline bool should_sleep(f32 linearSpeed, f32 angularSpeed, f32 linearThreshold, f32 angularThreshold) {
    if (!std::isfinite(linearSpeed) || !std::isfinite(angularSpeed)) {
        return false;
    }
    const auto limit = [](f32 threshold) {
        if (!std::isfinite(threshold) || threshold < 0.f) {
            return 0.f;
        }
        return threshold;
    };
    return linearSpeed <= limit(linearThreshold) && angularSpeed <= limit(angularThreshold);
}

} // namespace fuse::physics

#pragma once

#include <cmath>

namespace fuse::scene {

[[nodiscard]] inline float lod_distance(float error, float distance) {
    if (!std::isfinite(error) || !std::isfinite(distance) || distance <= 0.f) {
        return 0.f;
    }
    return error / distance;
}

} // namespace fuse::scene

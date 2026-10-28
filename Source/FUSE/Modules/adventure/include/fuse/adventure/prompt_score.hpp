#pragma once

// Interaction prompt score. Non-finite distance or range, a non-positive range,
// or a distance outside [0, range] yields 0. Otherwise priority falls off
// linearly from the full priority at distance 0 to 0 at distance == range.

#include <cmath>

namespace fuse::adventure {

[[nodiscard]] inline float prompt_score(float distance, float range, int priority) {
    if (!std::isfinite(distance) || !std::isfinite(range)) {
        return 0.f;
    }
    if (!(range > 0.f) || distance < 0.f || distance > range) {
        return 0.f;
    }
    return static_cast<float>(priority) * (1.f - distance / range);
}

} // namespace fuse::adventure

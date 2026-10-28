#pragma once

// Current health as a fraction of max in 0..1.
// Non-finite inputs, maxHealth <= 0, and a negative current return 0.
// Otherwise the result is saturate(current / maxHealth).

#include <cmath>

namespace fuse::mechanics {

[[nodiscard]] inline float health_ratio(float current, float maxHealth) {
    if (!std::isfinite(current) || !std::isfinite(maxHealth) || maxHealth <= 0.f || current < 0.f) {
        return 0.f;
    }
    const float t = current / maxHealth;
    return t > 1.f ? 1.f : t;
}

} // namespace fuse::mechanics

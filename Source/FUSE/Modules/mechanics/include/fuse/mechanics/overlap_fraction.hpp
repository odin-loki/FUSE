#pragma once

// Penetration depth as a fraction of size in 0..1.
// Non-finite inputs, size <= 0, and a negative depth return 0.
// Otherwise the result is saturate(depth / size).

#include <cmath>

namespace fuse::mechanics {

[[nodiscard]] inline float overlap_fraction(float depth, float size) {
    if (!std::isfinite(depth) || !std::isfinite(size) || size <= 0.f || depth < 0.f) {
        return 0.f;
    }
    const float t = depth / size;
    return t > 1.f ? 1.f : t;
}

} // namespace fuse::mechanics

#pragma once

// Normalized particle life in 0..1. Non-finite inputs, lifetime <= 0, and a negative age return 0.
// Otherwise the result is saturate(age / lifetime).

#include <cmath>

namespace fuse::vfx {

[[nodiscard]] inline float life_fraction(float age, float lifetime) {
    if (!std::isfinite(age) || !std::isfinite(lifetime) || lifetime <= 0.f || age < 0.f) {
        return 0.f;
    }
    const float t = age / lifetime;
    return t > 1.f ? 1.f : t;
}

} // namespace fuse::vfx

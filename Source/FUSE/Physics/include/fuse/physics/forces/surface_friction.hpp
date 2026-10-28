#pragma once

#include <fuse/physics/math.hpp>

#include <cmath>

namespace fuse::physics {

enum class FrictionCombine { Average, Multiply, Minimum };

/// Combines two surface friction coefficients.
/// Negative inputs clamp to 0. A non-finite input returns 0.
/// The result is not clamped to 1; a coefficient may be above 1.
[[nodiscard]] inline f32 combine_friction(f32 a, f32 b, FrictionCombine mode) {
    if (!std::isfinite(a) || !std::isfinite(b)) {
        return 0.f;
    }
    if (a < 0.f) {
        a = 0.f;
    }
    if (b < 0.f) {
        b = 0.f;
    }
    if (mode == FrictionCombine::Average) {
        return (a + b) / 2.f;
    }
    if (mode == FrictionCombine::Multiply) {
        return a * b;
    }
    if (mode == FrictionCombine::Minimum) {
        return a < b ? a : b;
    }
    return 0.f;
}

} // namespace fuse::physics

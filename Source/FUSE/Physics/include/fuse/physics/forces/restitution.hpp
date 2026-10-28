#pragma once

#include <fuse/physics/math.hpp>

#include <cmath>

namespace fuse::physics {

enum class RestitutionCombine { Average, Multiply, Maximum };

/// Each coefficient is clamped to 0..1 before the combine. Non-finite inputs become 0.
/// Average is (a + b) / 2, Multiply is a * b, and Maximum is the larger coefficient.
/// The result stays in 0..1.
[[nodiscard]] inline f32 combine_restitution(f32 a, f32 b, RestitutionCombine mode) {
    const auto clamp01 = [](f32 value) {
        if (!std::isfinite(value) || value <= 0.f) {
            return 0.f;
        }
        return value >= 1.f ? 1.f : value;
    };

    a = clamp01(a);
    b = clamp01(b);

    f32 combined = 0.f;
    switch (mode) {
    case RestitutionCombine::Average:
        combined = (a + b) / 2.f;
        break;
    case RestitutionCombine::Multiply:
        combined = a * b;
        break;
    case RestitutionCombine::Maximum:
        combined = a > b ? a : b;
        break;
    }

    if (!std::isfinite(combined) || combined <= 0.f) {
        return 0.f;
    }
    return combined >= 1.f ? 1.f : combined;
}

} // namespace fuse::physics

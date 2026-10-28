#pragma once

#include <cmath>

namespace fuse::platform {

struct StickAxis {
    float x;
    float y;
};

/// Radial stick deadzone. Non-finite `x`, `y`, or `deadzone` is (0, 0). `deadzone` is clamped
/// to [0, 0.99]. Magnitude 0 or below the deadzone is (0, 0). Deflection past the unit circle is
/// scaled onto it first, then the ring from the deadzone edge to length 1 is remapped onto 0..1:
/// scale = (length - deadzone) / (length * (1 - deadzone)).
[[nodiscard]] inline StickAxis apply_radial_deadzone(float x, float y, float deadzone) {
    if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(deadzone)) {
        return {0.f, 0.f};
    }
    if (deadzone < 0.f) {
        deadzone = 0.f;
    } else if (deadzone > 0.99f) {
        deadzone = 0.99f;
    }

    float length = std::hypot(x, y);
    if (!std::isfinite(length) || length == 0.f || length < deadzone) {
        return {0.f, 0.f};
    }
    if (length > 1.f) {
        x /= length;
        y /= length;
        length = 1.f;
    }
    const float scale = (length - deadzone) / (length * (1.f - deadzone));
    return {x * scale, y * scale};
}

} // namespace fuse::platform

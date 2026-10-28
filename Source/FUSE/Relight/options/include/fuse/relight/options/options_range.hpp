// FUSE Relight options: clamp a float option into a range, and treat a non-zero int as enabled.
//
// When minValue is greater than maxValue the bounds are swapped before the clamp. A non-finite
// value returns that minimum. option_enabled is an int check (value != 0); non-finite does not apply.
#pragma once

#include <cmath>

namespace fuse::relight::options {

/// Clamp `value` into [`minValue`, `maxValue`]. Bounds are swapped when `minValue` > `maxValue`.
/// A non-finite `value` returns the minimum after that swap.
[[nodiscard]] inline float clamp_option(float value, float minValue, float maxValue) {
    if (minValue > maxValue) {
        const float low = maxValue;
        maxValue = minValue;
        minValue = low;
    }
    if (!std::isfinite(value)) {
        return minValue;
    }
    if (value < minValue) {
        return minValue;
    }
    if (value > maxValue) {
        return maxValue;
    }
    return value;
}

/// A non-zero int option is enabled.
[[nodiscard]] inline bool option_enabled(int value) {
    return value != 0;
}

} // namespace fuse::relight::options

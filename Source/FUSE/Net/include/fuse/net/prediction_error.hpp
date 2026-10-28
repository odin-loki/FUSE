#pragma once

#include <cmath>

namespace fuse::net {

/// Absolute difference between a predicted value and the authoritative value.
/// Non-finite inputs return 0.
[[nodiscard]] inline float prediction_error(float predicted, float authoritative) noexcept {
    if (!std::isfinite(predicted) || !std::isfinite(authoritative)) {
        return 0.f;
    }
    return std::fabs(predicted - authoritative);
}

/// True when `error` is strictly above `threshold`.
/// A negative threshold counts as 0. A non-finite error returns false.
[[nodiscard]] inline bool needs_correction(float error, float threshold) noexcept {
    if (!std::isfinite(error)) {
        return false;
    }
    if (threshold < 0.f) {
        threshold = 0.f;
    }
    return error > threshold;
}

} // namespace fuse::net

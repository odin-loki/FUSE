#pragma once

#include <cmath>

namespace fuse::audio {

/// Spherical-head interaural time delay in seconds.
/// `(headWidthMeters * 0.5 * sin(azimuthRadians)) / speedOfSound`.
/// Returns 0 when any input is non-finite or `speedOfSound <= 0`.
/// Negative head width is treated as 0. Azimuth is not wrapped.
inline float interaural_delay_seconds(float azimuthRadians, float headWidthMeters,
                                      float speedOfSound) {
    if (!std::isfinite(azimuthRadians) || !std::isfinite(headWidthMeters)
        || !std::isfinite(speedOfSound) || speedOfSound <= 0.f) {
        return 0.f;
    }
    const float width = headWidthMeters < 0.f ? 0.f : headWidthMeters;
    return (width * 0.5f * std::sin(azimuthRadians)) / speedOfSound;
}

} // namespace fuse::audio

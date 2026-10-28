#pragma once

// Fixed-tick schedule budget: whole simulation ticks for a duration, and the
// seconds those ticks cover. Independent of the world runtime.

#include <cmath>
#include <cstdint>
#include <limits>

namespace fuse::world3d {

/// Whole ticks in `seconds` at `ticksPerSecond`.
/// Rounds half away from zero: floor(seconds * ticksPerSecond + 0.5).
/// Negative seconds, ticksPerSecond <= 0, or a non-finite input returns 0.
/// Counts above UINT32_MAX saturate.
[[nodiscard]] inline std::uint32_t ticks_for_seconds(float ticksPerSecond, float seconds) {
    if (!std::isfinite(ticksPerSecond) || !std::isfinite(seconds) || !(ticksPerSecond > 0.f) ||
        !(seconds >= 0.f)) {
        return 0u;
    }
    // Promote before the multiply so the count stays exact through UINT32_MAX.
    const double rounded =
        std::floor(static_cast<double>(seconds) * static_cast<double>(ticksPerSecond) + 0.5);
    constexpr double kLimit = static_cast<double>(std::numeric_limits<std::uint32_t>::max()) + 1.0;
    if (!(rounded < kLimit)) {
        return std::numeric_limits<std::uint32_t>::max();
    }
    return static_cast<std::uint32_t>(rounded);
}

/// Seconds covered by `ticks` at `ticksPerSecond` (`ticks / ticksPerSecond`).
/// ticksPerSecond <= 0 or non-finite returns 0.
[[nodiscard]] inline float seconds_for_ticks(std::uint32_t ticks, float ticksPerSecond) {
    if (!std::isfinite(ticksPerSecond) || !(ticksPerSecond > 0.f)) {
        return 0.f;
    }
    return static_cast<float>(ticks) / ticksPerSecond;
}

} // namespace fuse::world3d

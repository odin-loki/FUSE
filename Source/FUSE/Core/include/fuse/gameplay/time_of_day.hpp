#pragma once

#include <fuse/types.hpp>

#include <cmath>
#include <numbers>

namespace fuse::gameplay {

struct TimeOfDay {
    f32 seconds;
    f32 dayLength;
};

namespace detail {

/// A non-positive day length is one 24-hour day, in seconds.
[[nodiscard]] inline f32 timeOfDayLength(f32 dayLength) {
    return dayLength <= 0.f ? 86400.f : dayLength;
}

/// Wraps `seconds` into [0, length). Non-finite input is returned unchanged.
[[nodiscard]] inline f32 wrapDaySeconds(f32 seconds, f32 length) {
    f32 wrapped = std::fmod(seconds, length);
    if (!std::isfinite(wrapped)) {
        return wrapped;
    }
    if (wrapped < 0.f) {
        wrapped += length;
    }
    if (wrapped < 0.f || wrapped >= length) {
        return 0.f;
    }
    return wrapped;
}

} // namespace detail

/// Adds `dt` and wraps `seconds` into [0, dayLength). Negative `dt` wraps backward.
/// A non-finite `dt` leaves the clock unchanged. `dayLength <= 0` is a 86400-second day.
inline void advance(TimeOfDay& clock, f32 dt) {
    if (!std::isfinite(dt)) {
        return;
    }
    const f32 length = detail::timeOfDayLength(clock.dayLength);
    clock.seconds = detail::wrapDaySeconds(clock.seconds + dt, length);
}

/// `seconds / dayLength` wrapped into [0, 1).
[[nodiscard]] inline f32 normalized(const TimeOfDay& clock) {
    const f32 length = detail::timeOfDayLength(clock.dayLength);
    const f32 unit = detail::wrapDaySeconds(clock.seconds, length) / length;
    if (unit >= 1.f) {
        return 0.f;
    }
    return unit;
}

/// Sine of the day fraction: 0 at midnight, positive during the first half of the day.
[[nodiscard]] inline f32 sun_height(const TimeOfDay& clock) {
    constexpr f32 kTwoPi = 2.f * std::numbers::pi_v<f32>;
    return std::sin(kTwoPi * normalized(clock));
}

} // namespace fuse::gameplay

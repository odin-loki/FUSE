// FUSE Relight: pixel safe-zone inset for the developer overlay.
//
// Each edge moves in by `insetFraction` times that axis's window size (a title-safe margin). The fraction is clamped
// to [0, 0.45]. Width and height stay at least 1 when the window is non-zero, and the rectangle stays inside the
// window (x + width <= windowW, y + height <= windowH). A zero or negative window, or a non-finite fraction, returns
// a zero rectangle. The inset in pixels is fraction * window size, rounded to the nearest pixel.
#pragma once

#include <cmath>
#include <cstdint>

namespace fuse::relight::overlay {

struct SafeZoneRect {
    std::int32_t x = 0;
    std::int32_t y = 0;
    std::int32_t width = 0;
    std::int32_t height = 0;
};

namespace detail {

struct SafeZoneAxis {
    std::int32_t origin = 0;
    std::int32_t span = 0;
};

inline SafeZoneAxis safeZoneAxis(std::int32_t window, float fraction) {
    const double pixels = static_cast<double>(fraction) * static_cast<double>(window);
    // `fraction` is float, so an integral product can land just below the integer (0.45 * 100).
    std::int64_t inset = 0;
    if (pixels > 0.0) {
        inset = std::llround(pixels);
    }
    const std::int64_t limit = window;
    std::int64_t span = limit - inset - inset;
    if (span < 1) {
        span = 1;
    }
    std::int64_t origin = inset;
    if (origin + span > limit) {
        origin = limit - span;
    }
    if (origin < 0) {
        origin = 0;
    }
    return SafeZoneAxis{static_cast<std::int32_t>(origin), static_cast<std::int32_t>(span)};
}

} // namespace detail

/// Pixel rectangle left after insetting each edge by `insetFraction` of that axis.
[[nodiscard]] inline SafeZoneRect safe_zone_rect(std::int32_t windowW, std::int32_t windowH, float insetFraction) {
    if (windowW <= 0 || windowH <= 0 || !std::isfinite(insetFraction)) {
        return {};
    }
    float fraction = insetFraction;
    if (fraction < 0.f) {
        fraction = 0.f;
    } else if (fraction > 0.45f) {
        fraction = 0.45f;
    }
    const detail::SafeZoneAxis horizontal = detail::safeZoneAxis(windowW, fraction);
    const detail::SafeZoneAxis vertical = detail::safeZoneAxis(windowH, fraction);
    return SafeZoneRect{horizontal.origin, vertical.origin, horizontal.span, vertical.span};
}

} // namespace fuse::relight::overlay

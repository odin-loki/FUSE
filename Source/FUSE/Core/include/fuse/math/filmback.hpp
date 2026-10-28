#pragma once

#include <fuse/types.hpp>

#include <cmath>
#include <numbers>

namespace fuse::math {

/// Vertical field of view, in radians, that matches `horizontalFovRadians` on a frame of
/// `width` by `height`.
///
/// vertical = 2 * atan(tan(horizontalFovRadians / 2) * height / width).
/// Returns 0 when `width` or `height` is <= 0, when `horizontalFovRadians` is not in
/// (0, pi), or when any input or the result is non-finite.
[[nodiscard]] inline f32 vertical_fov_from_horizontal(f32 horizontalFovRadians, f32 width, f32 height) {
    if (!std::isfinite(horizontalFovRadians) || !std::isfinite(width) || !std::isfinite(height) || !(width > 0.f) ||
        !(height > 0.f)) {
        return 0.f;
    }

    constexpr f32 kPi = std::numbers::pi_v<f32>;
    if (!(horizontalFovRadians > 0.f) || !(horizontalFovRadians < kPi)) {
        return 0.f;
    }

    const f32 vertical = 2.f * std::atan(std::tan(horizontalFovRadians * 0.5f) * (height / width));
    return std::isfinite(vertical) ? vertical : 0.f;
}

} // namespace fuse::math

#pragma once

#include <fuse/math/vec.hpp>
#include <fuse/types.hpp>

#include <cmath>

namespace fuse::renderer {

namespace clear_color_detail {

/// Non-finite values become 0. Finite values outside [0, 1] clamp; in-range values pass through.
[[nodiscard]] inline f32 clamp01(f32 v) {
    if (!std::isfinite(v) || v < 0.f) {
        return 0.f;
    }
    if (v > 1.f) {
        return 1.f;
    }
    return v;
}

/// Non-finite values become 0. Finite values keep their magnitude so a later multiply can run first.
[[nodiscard]] inline f32 finiteOrZero(f32 v) { return std::isfinite(v) ? v : 0.f; }

} // namespace clear_color_detail

/// Straight-alpha clear colour to premultiplied RGBA.
/// Non-finite channels become 0. Alpha is clamped to [0, 1] before rgb is scaled by it, then every
/// channel is clamped to [0, 1].
[[nodiscard]] inline fuse::math::Vec4 premultiply_clear(fuse::math::Vec4 color) {
    const f32 r = clear_color_detail::finiteOrZero(color.x);
    const f32 g = clear_color_detail::finiteOrZero(color.y);
    const f32 b = clear_color_detail::finiteOrZero(color.z);
    const f32 a = clear_color_detail::clamp01(color.w);
    return {clear_color_detail::clamp01(r * a), clear_color_detail::clamp01(g * a), clear_color_detail::clamp01(b * a),
            clear_color_detail::clamp01(a)};
}

} // namespace fuse::renderer

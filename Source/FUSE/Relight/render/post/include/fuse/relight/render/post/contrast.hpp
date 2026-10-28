// FUSE Relight post: contrast around mid grey.
//
//   out = (value - 0.5) * contrast + 0.5
//
// Contrast is clamped to [0, 2]. The result is not clamped, so values may leave [0, 1]. A non-finite
// value yields 0. A non-finite contrast is treated as 1 (neutral).
#pragma once

#include <algorithm>
#include <cmath>

namespace fuse::relight::render::post {

/// `(value - 0.5) * contrast + 0.5`. Contrast is clamped to [0, 2]. See the file comment for the
/// non-finite rules. The result is not clamped.
[[nodiscard]] inline float apply_contrast(float value, float contrast) {
    if (!std::isfinite(value)) {
        return 0.f;
    }
    const float c = std::isfinite(contrast) ? std::clamp(contrast, 0.f, 2.f) : 1.f;
    return (value - 0.5f) * c + 0.5f;
}

} // namespace fuse::relight::render::post

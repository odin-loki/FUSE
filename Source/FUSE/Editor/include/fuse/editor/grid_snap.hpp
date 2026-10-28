#pragma once

#include <cmath>

namespace fuse::editor {

/// Nearest multiple of `grid`, with ties away from zero.
/// A non-positive or non-finite `grid` returns `value` when it is finite, otherwise 0.
/// A non-finite `value` returns 0.
[[nodiscard]] inline float snap_to_grid(float value, float grid) {
    if (!std::isfinite(grid) || !(grid > 0.f)) {
        return std::isfinite(value) ? value : 0.f;
    }
    if (!std::isfinite(value)) {
        return 0.f;
    }
    return std::round(value / grid) * grid;
}

} // namespace fuse::editor

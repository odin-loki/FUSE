#pragma once

#include <cmath>

namespace fuse::editor {

/// True when a viewport resize should be applied.
/// `width` and `height` must be positive. `debounceSeconds` and `secondsSinceLastResize`
/// must be finite, `debounceSeconds` must be at least zero, and the elapsed time must be
/// at least the debounce. A zero debounce fires immediately when the size is positive and
/// `secondsSinceLastResize` is finite and at least zero.
[[nodiscard]] inline bool resize_due(float secondsSinceLastResize, float debounceSeconds, int width,
                                     int height) {
    if (width <= 0 || height <= 0) {
        return false;
    }
    if (!std::isfinite(secondsSinceLastResize) || !std::isfinite(debounceSeconds)) {
        return false;
    }
    if (debounceSeconds < 0.f) {
        return false;
    }
    return secondsSinceLastResize >= debounceSeconds;
}

} // namespace fuse::editor

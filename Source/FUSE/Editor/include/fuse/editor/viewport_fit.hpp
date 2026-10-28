#pragma once

#include <cmath>

namespace fuse::editor {

/// Integer content rectangle in window pixels. Origin is the window's top-left.
struct ViewportRect {
    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;
};

/// Fit `contentAspect` (width / height) inside the window.
/// Non-finite or non-positive aspects use 16:9. A non-positive window returns a zero rect.
/// A wider window pillarboxes (height stays `windowH`); a taller window letterboxes (width stays
/// `windowW`). The fitted side is rounded to integer pixels and centered. The rect stays inside.
[[nodiscard]] inline ViewportRect fit_viewport(int windowW, int windowH, float contentAspect) {
    if (windowW <= 0 || windowH <= 0) {
        return {};
    }

    double aspect = static_cast<double>(contentAspect);
    if (!std::isfinite(aspect) || aspect <= 0.0) {
        aspect = 16.0 / 9.0;
    }

    // Round half away from zero, then clamp so the rect cannot leave the window.
    const auto roundInside = [](double value, int limit) -> int {
        if (!(value > 0.0) || !std::isfinite(value) || limit <= 0) {
            return 0;
        }
        if (value >= static_cast<double>(limit)) {
            return limit;
        }
        const long long rounded = std::llround(value);
        if (rounded <= 0) {
            return 0;
        }
        if (rounded >= static_cast<long long>(limit)) {
            return limit;
        }
        return static_cast<int>(rounded);
    };

    int width = windowW;
    int height = windowH;
    // Window aspect > content aspect: content is narrower than the window.
    if (static_cast<double>(windowW) > aspect * static_cast<double>(windowH)) {
        height = windowH;
        width = roundInside(static_cast<double>(windowH) * aspect, windowW);
    } else {
        width = windowW;
        height = roundInside(static_cast<double>(windowW) / aspect, windowH);
    }

    return ViewportRect{(windowW - width) / 2, (windowH - height) / 2, width, height};
}

} // namespace fuse::editor

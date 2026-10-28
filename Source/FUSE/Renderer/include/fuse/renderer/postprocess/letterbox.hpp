#pragma once

#include <fuse/types.hpp>

#include <cmath>

namespace fuse::renderer {

/// Content rectangle that fits `contentAspect` (width / height) inside a window, centered.
/// A wider window pillarboxes (bars on the left and right). A taller window letterboxes
/// (bars on the top and bottom). Leftover pixels are split; an odd pixel goes right or bottom.
struct LetterboxRect {
    u32 x;
    u32 y;
    u32 width;
    u32 height;
};

/// Integer pixel rect for `contentAspect` inside `windowW` x `windowH`.
/// Non-finite or non-positive aspects, and empty windows, return a zero rect.
/// Fitted width and height are at least 1 when the window is non-empty, and the rect stays inside it.
[[nodiscard]] inline LetterboxRect letterbox_rect(u32 windowW, u32 windowH, f32 contentAspect) {
    if (windowW == 0u || windowH == 0u || !std::isfinite(contentAspect) || contentAspect <= 0.f) {
        return {};
    }

    const double aspect = static_cast<double>(contentAspect);
    auto roundPixels = [](double value) -> u32 {
        if (!(value > 0.0) || !std::isfinite(value)) {
            return 0u;
        }
        const long long rounded = std::llround(value);
        if (rounded <= 0) {
            return 0u;
        }
        if (static_cast<unsigned long long>(rounded) > 0xffffffffull) {
            return 0xffffffffu;
        }
        return static_cast<u32>(rounded);
    };

    u32 width = windowW;
    u32 height = windowH;
    // Window aspect > content aspect: the content is narrower than the window.
    if (static_cast<double>(windowW) > aspect * static_cast<double>(windowH)) {
        height = windowH;
        width = roundPixels(static_cast<double>(windowH) * aspect);
    } else {
        width = windowW;
        height = roundPixels(static_cast<double>(windowW) / aspect);
    }

    if (width < 1u) {
        width = 1u;
    }
    if (height < 1u) {
        height = 1u;
    }
    if (width > windowW) {
        width = windowW;
    }
    if (height > windowH) {
        height = windowH;
    }

    return LetterboxRect{(windowW - width) / 2u, (windowH - height) / 2u, width, height};
}

} // namespace fuse::renderer

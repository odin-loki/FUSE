// FUSE Relight: corner snap for a developer-overlay box.
//
// The box's top-left sits on the chosen screen corner, inset by `margin` on both axes. A negative
// margin is treated as 0. TopLeft is (margin, margin). TopRight is (screenW - boxW - margin, margin).
// BottomLeft is (margin, screenH - boxH - margin). BottomRight uses both of those far edges. A
// non-positive screen or box, or any non-finite input (or a non-finite result), returns {0, 0}.
#pragma once

#include <cmath>

namespace fuse::relight::overlay {

enum class OverlayAnchor { TopLeft, TopRight, BottomLeft, BottomRight };

struct OverlayPos {
    float x = 0.f;
    float y = 0.f;
};

/// Top-left of a `boxW` x `boxH` box parked on `anchor`, inset by `margin`.
[[nodiscard]] inline OverlayPos snap_anchor(OverlayAnchor anchor, float screenW, float screenH, float boxW,
                                            float boxH, float margin) {
    if (!std::isfinite(screenW) || !std::isfinite(screenH) || !std::isfinite(boxW) || !std::isfinite(boxH) ||
        !std::isfinite(margin) || !(screenW > 0.f) || !(screenH > 0.f) || !(boxW > 0.f) || !(boxH > 0.f)) {
        return {0.f, 0.f};
    }
    if (margin < 0.f) {
        margin = 0.f;
    }
    float x = margin;
    float y = margin;
    switch (anchor) {
    case OverlayAnchor::TopLeft:
        break;
    case OverlayAnchor::TopRight:
        x = screenW - boxW - margin;
        break;
    case OverlayAnchor::BottomLeft:
        y = screenH - boxH - margin;
        break;
    case OverlayAnchor::BottomRight:
        x = screenW - boxW - margin;
        y = screenH - boxH - margin;
        break;
    }
    if (!std::isfinite(x) || !std::isfinite(y)) {
        return {0.f, 0.f};
    }
    return {x, y};
}

} // namespace fuse::relight::overlay

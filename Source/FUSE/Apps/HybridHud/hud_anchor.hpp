#pragma once

namespace fuse {

/// Horizontal origin of a HUD box pinned to the right edge.
/// A margin below 0 counts as 0. A non-positive screen or box width returns that margin.
/// Otherwise the origin is screenW - boxW - margin, clamped up to 0.
[[nodiscard]] constexpr int hud_anchor_x(int screenW, int boxW, int margin) {
    if (margin < 0) {
        margin = 0;
    }
    if (screenW <= 0 || boxW <= 0) {
        return margin;
    }
    const int origin = screenW - boxW - margin;
    return origin < 0 ? 0 : origin;
}

} // namespace fuse

#pragma once

#include <cmath>

namespace fuse::editor {

/// True when a screen-space pointer is within the gizmo hover radius.
/// `radiusPx` must be finite and in [1, 32]. `distancePx` must be finite and >= 0.
/// Any other input is not a hover.
[[nodiscard]] inline bool gizmo_hover_ok(float distancePx, float radiusPx) {
    if (!std::isfinite(radiusPx) || radiusPx < 1.f || radiusPx > 32.f) {
        return false;
    }
    if (!std::isfinite(distancePx) || distancePx < 0.f) {
        return false;
    }
    return distancePx <= radiusPx;
}

} // namespace fuse::editor

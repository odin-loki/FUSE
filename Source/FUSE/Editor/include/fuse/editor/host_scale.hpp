#pragma once

#include <cmath>

namespace fuse::editor {

/// True when `scale` is finite and within 1e-3 of 1, 1.25, 1.5, or 2.
[[nodiscard]] inline bool host_scale_ok(float scale) {
    if (!std::isfinite(scale)) {
        return false;
    }
    constexpr float kTolerance = 1e-3f;
    constexpr float kAllowed[] = {1.f, 1.25f, 1.5f, 2.f};
    for (const float allowed : kAllowed) {
        if (std::fabs(scale - allowed) <= kTolerance) {
            return true;
        }
    }
    return false;
}

} // namespace fuse::editor

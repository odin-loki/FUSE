#pragma once

#include <cmath>

namespace fuse::adventure {

/// True when the interact prompt is shown: enabled, looking at the target,
/// finite distance >= 0, finite useRange > 0, and distance <= useRange.
[[nodiscard]] inline bool interact_prompt_visible(float distance, float useRange, bool lookingAt,
                                                   bool enabled) {
    if (!enabled || !lookingAt || !std::isfinite(distance) || !std::isfinite(useRange)) {
        return false;
    }
    return distance >= 0.f && useRange > 0.f && distance <= useRange;
}

} // namespace fuse::adventure

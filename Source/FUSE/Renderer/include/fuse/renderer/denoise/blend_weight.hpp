#pragma once

#include <fuse/types.hpp>

#include <algorithm>
#include <cmath>

namespace fuse::renderer::denoise {

/// History blend weight clamped to [0, maxHistory]. maxHistory is clamped to [0, 1] first.
/// A non-finite history or maxHistory returns 0.
[[nodiscard]] inline f32 clamp_blend_weight(f32 history, f32 maxHistory) {
    if (!std::isfinite(history) || !std::isfinite(maxHistory)) {
        return 0.f;
    }
    return std::clamp(history, 0.f, std::clamp(maxHistory, 0.f, 1.f));
}

/// Current-frame blend weight: 1 minus historyWeight clamped to [0, 1].
/// A non-finite history weight returns 1.
[[nodiscard]] inline f32 blend_current(f32 historyWeight) {
    if (!std::isfinite(historyWeight)) {
        return 1.f;
    }
    return 1.f - std::clamp(historyWeight, 0.f, 1.f);
}

} // namespace fuse::renderer::denoise

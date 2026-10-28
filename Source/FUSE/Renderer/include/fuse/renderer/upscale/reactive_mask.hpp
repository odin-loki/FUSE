#pragma once

#include <cmath>

namespace fuse::renderer {

inline float reactive_mask_value(float previousLuma, float currentLuma, float threshold) {
    if (!std::isfinite(previousLuma) || !std::isfinite(currentLuma) || !std::isfinite(threshold)) {
        return 0.f;
    }
    if (threshold < 0.f) {
        threshold = 0.f;
    }
    const float delta = std::fabs(currentLuma - previousLuma);
    if (delta <= threshold) {
        return 0.f;
    }
    const float denom = threshold > 1e-4f ? threshold : 1e-4f;
    float value = (delta - threshold) / denom;
    if (value < 0.f) {
        value = 0.f;
    }
    if (value > 1.f) {
        value = 1.f;
    }
    return value;
}

} // namespace fuse::renderer

#pragma once

#include <algorithm>
#include <cmath>

namespace fuse::audio {

/// Scale \p sample by a decibel bus gain: linear = pow(10, gainDb / 20).
/// gainDb is clamped to [-80, 24] before the power. A non-finite sample or gainDb returns 0.
[[nodiscard]] inline float apply_bus_gain(float sample, float gainDb) {
    if (!std::isfinite(sample) || !std::isfinite(gainDb)) {
        return 0.f;
    }
    gainDb = std::clamp(gainDb, -80.f, 24.f);
    const float linear = std::pow(10.f, gainDb / 20.f);
    return sample * linear;
}

} // namespace fuse::audio

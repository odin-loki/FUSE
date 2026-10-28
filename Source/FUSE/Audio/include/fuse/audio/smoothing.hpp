#pragma once

#include <algorithm>
#include <cmath>

namespace fuse::audio {

/// One-pole blend coefficient `1 - exp(-dt / timeSeconds)`, clamped to [0, 1].
/// Non-finite or non-positive `dt` returns 0. `timeSeconds <= 0` returns 1.
[[nodiscard]] inline float smoothing_alpha(float dt, float timeSeconds) {
    if (!std::isfinite(dt) || dt <= 0.f) {
        return 0.f;
    }
    if (timeSeconds <= 0.f) {
        return 1.f;
    }
    const float alpha = 1.f - std::exp(-dt / timeSeconds);
    if (!std::isfinite(alpha)) {
        return 0.f;
    }
    return std::clamp(alpha, 0.f, 1.f);
}

} // namespace fuse::audio

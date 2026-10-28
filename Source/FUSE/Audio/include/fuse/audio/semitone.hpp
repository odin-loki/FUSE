#pragma once

#include <algorithm>
#include <cmath>

namespace fuse::audio {

/// Frequency ratio for a pitch shift of \p semitones: pow(2, semitones / 12).
/// Non-finite input returns 1. Semitones are clamped to [-48, 48] before the power.
[[nodiscard]] inline float semitone_ratio(float semitones) {
    if (!std::isfinite(semitones)) {
        return 1.f;
    }
    semitones = std::clamp(semitones, -48.f, 48.f);
    return std::pow(2.f, semitones / 12.f);
}

} // namespace fuse::audio

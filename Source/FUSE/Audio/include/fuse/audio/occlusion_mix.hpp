#pragma once

#include <algorithm>
#include <cmath>

namespace fuse::audio {

/// Occlusion mix from source visibility: `1 - clamp(visibility, 0, 1)`.
/// Non-finite visibility returns 0.
[[nodiscard]] inline float occlusion_mix(float visibility) {
    if (!std::isfinite(visibility)) {
        return 0.f;
    }
    visibility = std::clamp(visibility, 0.f, 1.f);
    return 1.f - visibility;
}

} // namespace fuse::audio

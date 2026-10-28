#pragma once

#include <cmath>

namespace fuse::audio {

/// True when \p degrees is finite and in [-180, 180].
[[nodiscard]] inline bool azimuth_ok(float degrees) {
    return std::isfinite(degrees) && degrees >= -180.f && degrees <= 180.f;
}

} // namespace fuse::audio

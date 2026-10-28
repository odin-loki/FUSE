#pragma once

#include <cmath>

namespace fuse::cinematics {

/// True when `seconds` is finite and in [0, 3600].
[[nodiscard]] inline bool cue_time_ok(float seconds) {
    return std::isfinite(seconds) && seconds >= 0.f && seconds <= 3600.f;
}

} // namespace fuse::cinematics

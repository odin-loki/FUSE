#pragma once

namespace fuse::animation {

/// True when crossfade duration in milliseconds is in 0..5000.
[[nodiscard]] inline bool crossfade_ms_ok(int ms) {
    return ms >= 0 && ms <= 5000;
}

} // namespace fuse::animation

#pragma once

namespace fuse::cinematics {

/// True when a shot duration is in the inclusive range 1..600000 ms (ten minutes).
[[nodiscard]] inline bool shot_duration_ok(int durationMs) {
    return durationMs >= 1 && durationMs <= 600000;
}

} // namespace fuse::cinematics

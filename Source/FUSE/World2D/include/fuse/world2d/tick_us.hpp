#pragma once

namespace fuse::world2d {

/// True when a tick duration is in the inclusive range 0..50000 microseconds.
[[nodiscard]] inline bool tick_us_ok(int microseconds) {
    return microseconds >= 0 && microseconds <= 50000;
}

} // namespace fuse::world2d

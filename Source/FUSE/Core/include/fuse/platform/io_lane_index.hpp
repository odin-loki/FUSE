#pragma once

namespace fuse::platform {

/// True when `lane` is in the inclusive range 0..7.
[[nodiscard]] inline bool io_lane_ok(int lane) {
    return lane >= 0 && lane <= 7;
}

} // namespace fuse::platform

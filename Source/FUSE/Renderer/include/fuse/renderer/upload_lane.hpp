#pragma once

namespace fuse::renderer {

/// True when `lane` is in the inclusive range 0..7.
[[nodiscard]] inline bool upload_lane_ok(int lane) {
    return lane >= 0 && lane <= 7;
}

} // namespace fuse::renderer

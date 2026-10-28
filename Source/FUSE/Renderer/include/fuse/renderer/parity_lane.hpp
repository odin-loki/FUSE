#pragma once

namespace fuse::renderer {

/// True only for lanes 0 through 15 inclusive.
[[nodiscard]] inline bool parity_lane_ok(int lane) {
    return lane >= 0 && lane <= 15;
}

} // namespace fuse::renderer

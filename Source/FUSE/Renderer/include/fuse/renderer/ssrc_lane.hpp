#pragma once

namespace fuse::renderer {

/// True only for lanes 0 through 7 inclusive.
[[nodiscard]] inline bool ssrc_lane_ok(int lane) {
    return lane >= 0 && lane <= 7;
}

} // namespace fuse::renderer

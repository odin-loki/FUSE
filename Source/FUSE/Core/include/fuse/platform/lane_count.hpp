#pragma once

namespace fuse::platform {

/// True when `count` is in the inclusive range 1..8.
[[nodiscard]] inline bool lane_count_ok(int count) {
    return count >= 1 && count <= 8;
}

} // namespace fuse::platform

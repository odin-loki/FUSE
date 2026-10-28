#pragma once

namespace fuse::physics {

/// True when a substep count is in the inclusive range 1..16.
[[nodiscard]] inline bool substep_count_ok(int count) {
    return count >= 1 && count <= 16;
}

} // namespace fuse::physics

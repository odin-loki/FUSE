#pragma once

namespace fuse::mechanics {

/// True when a spawn count is in the inclusive range 1..64.
[[nodiscard]] inline bool spawn_count_ok(int count) {
    return count >= 1 && count <= 64;
}

} // namespace fuse::mechanics

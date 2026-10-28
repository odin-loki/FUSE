#pragma once

namespace fuse::ai {

/// True when a parallel child count is in the inclusive range 1..16.
[[nodiscard]] inline bool child_count_ok(int count) {
    return count >= 1 && count <= 16;
}

} // namespace fuse::ai

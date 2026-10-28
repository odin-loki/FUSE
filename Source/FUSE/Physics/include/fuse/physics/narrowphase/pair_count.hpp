#pragma once

namespace fuse::physics {

/// True when a pair count is in the inclusive range 0..1024.
[[nodiscard]] inline bool pair_count_ok(int pairs) {
    return pairs >= 0 && pairs <= 1024;
}

} // namespace fuse::physics

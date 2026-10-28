#pragma once

namespace fuse::adventure {

/// True when `count` is in 1..12.
[[nodiscard]] inline bool objective_count_ok(int count) {
    return count >= 1 && count <= 12;
}

} // namespace fuse::adventure

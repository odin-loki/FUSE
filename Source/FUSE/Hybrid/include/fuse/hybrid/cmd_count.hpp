#pragma once

namespace fuse::hybrid {

/// True when count is in 0..(1 << 20).
[[nodiscard]] inline bool cmd_count_ok(int count) {
    return count >= 0 && count <= (1 << 20);
}

} // namespace fuse::hybrid

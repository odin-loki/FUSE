#pragma once

namespace fuse::renderer {

/// True when cascade count is in 1..4.
[[nodiscard]] inline bool cascade_count_ok(int count) {
    return count >= 1 && count <= 4;
}

} // namespace fuse::renderer

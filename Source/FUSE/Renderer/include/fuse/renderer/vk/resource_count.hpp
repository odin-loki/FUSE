#pragma once

namespace fuse::renderer {

/// True only for counts 1 through (1<<20) inclusive.
[[nodiscard]] inline bool resource_count_ok(int count) {
    return count >= 1 && count <= (1 << 20);
}

} // namespace fuse::renderer

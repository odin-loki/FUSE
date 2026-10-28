#pragma once

namespace fuse::renderer {

/// True only when `count` is in the closed range [1, 4096].
[[nodiscard]] inline bool cluster_count_ok(int count) {
    return count >= 1 && count <= 4096;
}

} // namespace fuse::renderer

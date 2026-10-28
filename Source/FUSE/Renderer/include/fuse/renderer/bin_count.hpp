#pragma once

namespace fuse::renderer {

/// True only when `count` is in the closed range [1, 128].
[[nodiscard]] inline bool bin_count_ok(int count) {
    return count >= 1 && count <= 128;
}

} // namespace fuse::renderer

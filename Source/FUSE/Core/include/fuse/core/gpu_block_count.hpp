#pragma once

namespace fuse::core {

/// True when `count` is in the closed range [1, 4096].
[[nodiscard]] inline bool gpu_block_count_ok(int count) {
    return count >= 1 && count <= 4096;
}

} // namespace fuse::core

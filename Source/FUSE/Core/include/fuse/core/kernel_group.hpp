#pragma once

namespace fuse::core {

/// True when `size` is a power of two in [32, 256].
[[nodiscard]] inline bool kernel_group_ok(int size) {
    return size >= 32 && size <= 256 && (size & (size - 1)) == 0;
}

} // namespace fuse::core

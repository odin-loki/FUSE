#pragma once

namespace fuse::core {

/// True when `align` is a power of two in [1, 256].
[[nodiscard]] inline bool alloc_align_ok(int align) {
    return align >= 1 && align <= 256 && (align & (align - 1)) == 0;
}

} // namespace fuse::core

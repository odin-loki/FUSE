#pragma once

namespace fuse::physics {

/// True when count is a power of two in [16, 4096].
[[nodiscard]] inline bool bucket_count_ok(int count) {
    if (count < 16 || count > 4096) {
        return false;
    }
    return (count & (count - 1)) == 0;
}

} // namespace fuse::physics

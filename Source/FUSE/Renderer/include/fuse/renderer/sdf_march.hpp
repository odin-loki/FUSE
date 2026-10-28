#pragma once

namespace fuse::renderer {

/// True only when `n` is a power of two from 16 through 128.
/// Zero is false, because a power-of-two test must require n > 0.
[[nodiscard]] inline bool sdf_march_ok(int n) {
    if (!(n > 0)) {
        return false;
    }
    if (n < 16 || n > 128) {
        return false;
    }
    return (n & (n - 1)) == 0;
}

} // namespace fuse::renderer

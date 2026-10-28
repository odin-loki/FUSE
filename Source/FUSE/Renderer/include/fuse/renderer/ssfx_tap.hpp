#pragma once

namespace fuse::renderer {

/// True only when `n` is a power of two from 4 through 32.
/// Zero is false, because a power-of-two test must require n > 0.
[[nodiscard]] inline bool ssfx_tap_ok(int n) {
    if (!(n > 0)) {
        return false;
    }
    if (n < 4 || n > 32) {
        return false;
    }
    return (n & (n - 1)) == 0;
}

} // namespace fuse::renderer

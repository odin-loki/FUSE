#pragma once

namespace fuse::scene {

/// True when `count` is a power of two in the closed interval [1, 64].
[[nodiscard]] inline bool mis_samples_ok(int count) {
    if (count < 1 || count > 64) {
        return false;
    }
    return (count & (count - 1)) == 0;
}

} // namespace fuse::scene

#pragma once

namespace fuse::renderer {

/// True when `count` is a power of two in the inclusive range 4..64.
[[nodiscard]] inline bool ltc_sample_ok(int count) {
    if (count < 4 || count > 64) {
        return false;
    }
    return (count & (count - 1)) == 0;
}

} // namespace fuse::renderer

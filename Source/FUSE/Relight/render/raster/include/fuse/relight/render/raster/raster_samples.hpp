#pragma once

namespace fuse::relight {

/// True when `count` is a power of two in [1, 16].
[[nodiscard]] inline bool raster_samples_ok(int count) {
    if (count < 1 || count > 16) {
        return false;
    }
    return (count & (count - 1)) == 0;
}

} // namespace fuse::relight

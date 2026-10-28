// FUSE Relight: probe count is a power of two in [1, 256].
#pragma once

namespace fuse::relight {

/// True when `count` is a power of two in the closed interval [1, 256].
[[nodiscard]] inline bool probe_count_ok(int count) {
    if (count < 1 || count > 256) {
        return false;
    }
    return (count & (count - 1)) == 0;
}

} // namespace fuse::relight

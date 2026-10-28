// Reservoir count: a count is valid only when it lies in [1, 64].
#pragma once

namespace fuse::relight {

/// True when `count` lies in [1, 64].
[[nodiscard]] inline bool reservoir_count_ok(int count) {
    return count >= 1 && count <= 64;
}

} // namespace fuse::relight

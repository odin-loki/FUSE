// Spark count: a count is valid only when it lies in [0, 100000].
#pragma once

namespace fuse::relight {

/// True when `count` is in the inclusive range 0..100000.
[[nodiscard]] inline bool spark_count_ok(int count) {
    return count >= 0 && count <= 100000;
}

} // namespace fuse::relight

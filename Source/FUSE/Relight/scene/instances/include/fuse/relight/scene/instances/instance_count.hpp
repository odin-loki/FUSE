#pragma once

namespace fuse::relight {

/// True only for counts 1 through 1000000 inclusive.
[[nodiscard]] inline bool instance_count_ok(int count) {
    return count >= 1 && count <= 1000000;
}

} // namespace fuse::relight

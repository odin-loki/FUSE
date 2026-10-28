#pragma once

namespace fuse::core {

/// True when `cycles` is in the closed range [1, 1000000].
[[nodiscard]] inline bool cycle_count_ok(int cycles) {
    return cycles >= 1 && cycles <= 1000000;
}

} // namespace fuse::core

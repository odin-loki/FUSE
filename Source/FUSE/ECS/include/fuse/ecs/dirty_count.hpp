#pragma once

namespace fuse::ecs {

/// True when `count` is in the inclusive range 0..100000.
[[nodiscard]] inline constexpr bool dirty_count_ok(int count) {
    return count >= 0 && count <= 100000;
}

} // namespace fuse::ecs

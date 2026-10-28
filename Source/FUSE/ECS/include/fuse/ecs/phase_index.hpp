#pragma once

namespace fuse::ecs {

/// True when `index` is in the inclusive range 0..2.
[[nodiscard]] inline constexpr bool phase_index_ok(int index) {
    return index >= 0 && index <= 2;
}

} // namespace fuse::ecs

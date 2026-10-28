#pragma once

namespace fuse::ecs {

/// True when `depth` is in the inclusive range 1..32.
[[nodiscard]] inline constexpr bool bvh_depth_ok(int depth) {
    return depth >= 1 && depth <= 32;
}

} // namespace fuse::ecs

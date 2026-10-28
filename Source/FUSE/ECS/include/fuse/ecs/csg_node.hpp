#pragma once

namespace fuse::ecs {

/// True when `nodes` is in the inclusive range 1..64.
[[nodiscard]] inline constexpr bool csg_node_ok(int nodes) {
    return nodes >= 1 && nodes <= 64;
}

} // namespace fuse::ecs

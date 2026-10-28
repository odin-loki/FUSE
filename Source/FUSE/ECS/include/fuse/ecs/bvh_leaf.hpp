#pragma once

namespace fuse::ecs {

/// True when a BVH leaf primitive count is in the inclusive range 1..8.
[[nodiscard]] inline bool bvh_leaf_ok(int count) {
    return count >= 1 && count <= 8;
}

} // namespace fuse::ecs

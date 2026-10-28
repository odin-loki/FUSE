#pragma once

namespace fuse::ai {

/// True when a behavior-tree depth is in the inclusive range 1..32.
[[nodiscard]] inline bool bt_depth_ok(int depth) {
    return depth >= 1 && depth <= 32;
}

} // namespace fuse::ai

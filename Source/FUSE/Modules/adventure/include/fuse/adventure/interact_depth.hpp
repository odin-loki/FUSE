#pragma once

namespace fuse::adventure {

/// True only for depths 1 through 32 inclusive.
[[nodiscard]] inline bool interact_depth_ok(int depth) {
    return depth >= 1 && depth <= 32;
}

} // namespace fuse::adventure

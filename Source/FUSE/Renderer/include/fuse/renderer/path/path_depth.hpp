#pragma once

namespace fuse::renderer {

/// True when `depth` is in the inclusive range 1..16.
[[nodiscard]] inline bool path_depth_ok(int depth) {
    return depth >= 1 && depth <= 16;
}

} // namespace fuse::renderer

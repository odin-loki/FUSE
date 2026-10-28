#pragma once

namespace fuse::renderer {

/// True only for depths 1 through 32 inclusive.
[[nodiscard]] inline bool light_depth_ok(int depth) {
    return depth >= 1 && depth <= 32;
}

} // namespace fuse::renderer

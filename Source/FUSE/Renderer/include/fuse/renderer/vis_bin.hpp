#pragma once

namespace fuse::renderer {

/// True only for bin counts 1 through 32 inclusive.
[[nodiscard]] inline bool vis_bin_ok(int count) {
    return count >= 1 && count <= 32;
}

} // namespace fuse::renderer

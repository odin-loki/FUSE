#pragma once

namespace fuse::renderer {

/// True only for bin counts 1 through 64 inclusive.
[[nodiscard]] inline bool sw_bin_ok(int count) {
    return count >= 1 && count <= 64;
}

} // namespace fuse::renderer

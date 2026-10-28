#pragma once

namespace fuse::world3d {

/// True when a parent may have this many children, including none.
[[nodiscard]] inline bool sibling_count_ok(int count) {
    return count >= 0 && count <= 256;
}

} // namespace fuse::world3d

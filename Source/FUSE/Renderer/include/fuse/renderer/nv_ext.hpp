#pragma once

namespace fuse::renderer {

/// True only for indexes 0 through 15 inclusive.
[[nodiscard]] inline bool nv_ext_ok(int index) {
    return index >= 0 && index <= 15;
}

} // namespace fuse::renderer

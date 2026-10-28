#pragma once

namespace fuse::renderer {

/// True only for indexes 0 through 15 inclusive.
[[nodiscard]] inline bool device_index_ok(int index) {
    return index >= 0 && index <= 15;
}

} // namespace fuse::renderer

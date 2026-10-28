#pragma once

namespace fuse::platform {

/// True when `index` is in the closed range 0..255.
[[nodiscard]] inline bool key_index_ok(int index) {
    return index >= 0 && index <= 255;
}

} // namespace fuse::platform

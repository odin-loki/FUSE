#pragma once

namespace fuse::renderer {

/// True only for indexes 0 through 7 inclusive.
[[nodiscard]] inline bool ddgi_device_ok(int index) {
    return index >= 0 && index <= 7;
}

} // namespace fuse::renderer

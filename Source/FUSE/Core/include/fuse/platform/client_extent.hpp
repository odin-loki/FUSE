#pragma once

namespace fuse::platform {

/// True when both `width` and `height` are in the closed range 1..16384.
[[nodiscard]] inline bool client_extent_ok(int width, int height) {
    return width >= 1 && width <= 16384 && height >= 1 && height <= 16384;
}

} // namespace fuse::platform

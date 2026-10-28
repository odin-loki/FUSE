#pragma once

namespace fuse::hybrid {

/// True when both dimensions are positive and at most 8192.
[[nodiscard]] inline bool present_size_ok(int width, int height) {
    return width > 0 && height > 0 && width <= 8192 && height <= 8192;
}

} // namespace fuse::hybrid

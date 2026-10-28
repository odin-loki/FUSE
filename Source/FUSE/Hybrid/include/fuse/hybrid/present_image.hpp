#pragma once

namespace fuse::hybrid {

/// True only for image counts 1 through 8 inclusive.
[[nodiscard]] inline bool present_image_ok(int count) {
    return count >= 1 && count <= 8;
}

} // namespace fuse::hybrid

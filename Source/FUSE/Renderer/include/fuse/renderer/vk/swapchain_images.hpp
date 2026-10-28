#pragma once

namespace fuse::renderer {

/// True only for image counts 2 through 8 inclusive.
[[nodiscard]] inline bool swapchain_images_ok(int count) {
    return count >= 2 && count <= 8;
}

} // namespace fuse::renderer

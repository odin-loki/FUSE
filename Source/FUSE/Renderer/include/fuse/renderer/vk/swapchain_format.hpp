#pragma once

namespace fuse::renderer {

/// True only for formats 0 through 7 inclusive.
[[nodiscard]] inline bool swapchain_format_ok(int format) {
    return format >= 0 && format <= 7;
}

} // namespace fuse::renderer

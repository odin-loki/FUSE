#pragma once

namespace fuse::editor {

/// True when both resize edges lie in [1, 8192].
[[nodiscard]] inline bool resize_px_ok(int width, int height) {
    return width >= 1 && width <= 8192 && height >= 1 && height <= 8192;
}

} // namespace fuse::editor

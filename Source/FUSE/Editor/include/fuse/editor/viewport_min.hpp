#pragma once

namespace fuse::editor {

/// True when both viewport edges are at least 64 pixels.
[[nodiscard]] inline bool viewport_min_ok(int width, int height) {
    return width >= 64 && height >= 64;
}

} // namespace fuse::editor

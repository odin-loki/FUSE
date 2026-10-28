#pragma once

namespace fuse::editor {

/// True only for depths 1 through 256 inclusive.
[[nodiscard]] inline bool undo_depth_ok(int depth) {
    return depth >= 1 && depth <= 256;
}

} // namespace fuse::editor

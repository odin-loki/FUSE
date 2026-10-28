#pragma once

namespace fuse::editor {

/// True when `depth` lies in `[0, 512]`.
[[nodiscard]] inline bool redo_depth_ok(int depth) {
    return depth >= 0 && depth <= 512;
}

} // namespace fuse::editor

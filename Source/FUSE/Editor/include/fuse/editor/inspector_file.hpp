#pragma once

namespace fuse::editor {

/// True when `count` is in [0, 64].
[[nodiscard]] inline bool inspector_file_ok(int count) {
    return count >= 0 && count <= 64;
}

} // namespace fuse::editor

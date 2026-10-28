#pragma once

namespace fuse::editor {

/// True when `count` is in the inclusive range 1..64.
[[nodiscard]] inline bool scene_file_count_ok(int count) {
    return count >= 1 && count <= 64;
}

} // namespace fuse::editor

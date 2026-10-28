#pragma once

namespace fuse::editor {

/// True when `count` is in [0, 100000] and `row` lies in [0, count).
[[nodiscard]] inline bool hierarchy_row_ok(int row, int count) {
    return count >= 0 && count <= 100000 && row >= 0 && row < count;
}

} // namespace fuse::editor

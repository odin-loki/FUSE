#pragma once

namespace fuse::editor {

/// True when `row` is in [0, 512].
[[nodiscard]] inline bool inspector_row_ok(int row) {
    return row >= 0 && row <= 512;
}

} // namespace fuse::editor

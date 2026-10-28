#pragma once

namespace fuse::editor {

/// True when `index` is in [0, 15].
[[nodiscard]] inline bool panel_index_ok(int index) {
    return index >= 0 && index <= 15;
}

} // namespace fuse::editor

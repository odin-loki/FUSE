#pragma once

namespace fuse::editor {

/// Tab slot when `count` is positive and `index` lies in `0 .. count - 1`.
/// Otherwise -1.
[[nodiscard]] inline int panel_tab_index(int index, int count) {
    if (count > 0 && index >= 0 && index < count) {
        return index;
    }
    return -1;
}

/// Next tab, wrapping the last tab to 0. `count <= 0` returns -1.
/// An out-of-range index starts at 0 when `count > 0`.
[[nodiscard]] inline int panel_tab_next(int index, int count) {
    if (count <= 0) {
        return -1;
    }
    if (index < 0 || index >= count) {
        return 0;
    }
    if (index + 1 >= count) {
        return 0;
    }
    return index + 1;
}

} // namespace fuse::editor

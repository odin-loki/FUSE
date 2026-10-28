#pragma once

namespace fuse::editor {

/// True only for panes 0 through 7 inclusive.
[[nodiscard]] inline bool host_pane_ok(int pane) {
    return pane >= 0 && pane <= 7;
}

} // namespace fuse::editor

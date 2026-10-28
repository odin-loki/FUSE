#pragma once

namespace fuse::editor {

/// True when `mode` is 0, 1, or 2.
[[nodiscard]] inline bool play_mode_ok(int mode) {
    return mode == 0 || mode == 1 || mode == 2;
}

} // namespace fuse::editor

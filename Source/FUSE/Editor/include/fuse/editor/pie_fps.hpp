#pragma once

namespace fuse::editor {

/// True when play-in-editor runs at 30 or 60 frames per second.
[[nodiscard]] inline bool pie_fps_ok(int fps) {
    return fps == 30 || fps == 60;
}

} // namespace fuse::editor

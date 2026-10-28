#pragma once

namespace fuse::editor {

/// True only when a play session runs at 30 or 60 Hz.
[[nodiscard]] inline bool session_hz_ok(int hz) {
    return hz == 30 || hz == 60;
}

} // namespace fuse::editor

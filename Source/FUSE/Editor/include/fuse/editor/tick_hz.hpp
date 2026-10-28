#pragma once

namespace fuse::editor {

[[nodiscard]] inline bool tick_hz_ok(int hz) {
    return hz == 10 || hz == 20 || hz == 30 || hz == 60 || hz == 120;
}

} // namespace fuse::editor

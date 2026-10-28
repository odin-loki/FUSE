#pragma once

namespace fuse::editor {

/// Map a requested vsync frame count to a present interval.
/// Zero or negative is immediate (interval 0). A positive count is clamped to 1..4.
[[nodiscard]] inline int present_interval(int vsyncFrames) {
    if (vsyncFrames <= 0) {
        return 0;
    }
    if (vsyncFrames > 4) {
        return 4;
    }
    return vsyncFrames;
}

/// True when `interval` waits at least one vertical blank. Interval 0 is immediate.
[[nodiscard]] inline bool present_is_fifo(int interval) {
    return interval >= 1;
}

} // namespace fuse::editor

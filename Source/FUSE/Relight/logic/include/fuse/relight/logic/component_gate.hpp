#pragma once

namespace fuse::relight::logic {

/// True when the component and its parent are enabled and the start delay has elapsed.
/// A negative delay is treated as zero, so it does not block.
[[nodiscard]] inline bool component_runs(bool enabled, bool parentEnabled, int delayFrames) {
    const int delay = delayFrames < 0 ? 0 : delayFrames;
    return enabled && parentEnabled && delay <= 0;
}

/// Frames of start delay remaining after this frame. Zero once the delay has elapsed.
[[nodiscard]] inline int component_delay_left(int delayFrames) {
    if (delayFrames > 0) {
        return delayFrames - 1;
    }
    return 0;
}

} // namespace fuse::relight::logic

#pragma once

namespace fuse::editor {

/// True when a paused session should skip its tick. Returns `paused`.
[[nodiscard]] inline bool pause_blocks_tick(bool paused) {
    return paused;
}

} // namespace fuse::editor

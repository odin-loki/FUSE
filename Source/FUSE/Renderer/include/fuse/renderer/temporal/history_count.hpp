#pragma once

namespace fuse::renderer {

/// True when temporal history count is in 1..16.
[[nodiscard]] inline bool temporal_history_ok(int count) {
    return count >= 1 && count <= 16;
}

} // namespace fuse::renderer

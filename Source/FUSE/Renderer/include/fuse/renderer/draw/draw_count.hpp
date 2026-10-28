#pragma once

namespace fuse::renderer {

/// True when draw count is in 0..(1 << 20).
[[nodiscard]] inline bool draw_count_ok(int count) {
    return count >= 0 && count <= (1 << 20);
}

} // namespace fuse::renderer

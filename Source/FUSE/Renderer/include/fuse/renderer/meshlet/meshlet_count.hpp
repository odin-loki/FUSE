#pragma once

namespace fuse::renderer {

/// True when meshlet count is in 1..(1<<20).
[[nodiscard]] inline bool meshlet_count_ok(int count) {
    return count >= 1 && count <= (1 << 20);
}

} // namespace fuse::renderer

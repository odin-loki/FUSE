#pragma once

// CPU check for pipeline cache entry counts. Valid range is 1..(1<<20).

namespace fuse::renderer {

[[nodiscard]] inline bool cache_entries_ok(int count) {
    return count >= 1 && count <= (1 << 20);
}

} // namespace fuse::renderer

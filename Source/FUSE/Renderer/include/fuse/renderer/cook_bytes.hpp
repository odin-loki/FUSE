#pragma once

namespace fuse::renderer {

/// True only for byte counts 1 through (1<<20) inclusive.
[[nodiscard]] inline bool cook_bytes_ok(int bytes) {
    return bytes >= 1 && bytes <= (1 << 20);
}

} // namespace fuse::renderer

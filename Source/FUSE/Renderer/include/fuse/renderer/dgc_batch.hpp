#pragma once

namespace fuse::renderer {

/// True when `count` is in the inclusive range 1..256.
[[nodiscard]] inline bool dgc_batch_ok(int count) {
    return count >= 1 && count <= 256;
}

} // namespace fuse::renderer

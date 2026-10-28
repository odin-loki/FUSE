#pragma once

namespace fuse::renderer {

/// True when `count` is in the inclusive range 1..4096.
[[nodiscard]] inline bool dgc_draw_ok(int count) {
    return count >= 1 && count <= 4096;
}

} // namespace fuse::renderer

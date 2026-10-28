#pragma once

namespace fuse::renderer {

/// True only for tap counts 1 through 16.
[[nodiscard]] inline bool ssrc_tap_ok(int count) {
    return count >= 1 && count <= 16;
}

} // namespace fuse::renderer

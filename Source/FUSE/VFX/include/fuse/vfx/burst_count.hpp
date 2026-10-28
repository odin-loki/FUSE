#pragma once

namespace fuse::vfx {

/// True when `count` is in the closed range [1, 64].
[[nodiscard]] inline bool burst_count_ok(int count) {
    return count >= 1 && count <= 64;
}

} // namespace fuse::vfx

#pragma once

namespace fuse::vfx {

/// True when `count` is in the closed range [0, 4096].
[[nodiscard]] inline bool vfx_count_ok(int count) {
    return count >= 0 && count <= 4096;
}

} // namespace fuse::vfx

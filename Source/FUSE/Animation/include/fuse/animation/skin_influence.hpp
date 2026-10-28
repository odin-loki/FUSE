#pragma once

namespace fuse::animation {

/// True when `count` is in the closed range 1..4.
[[nodiscard]] inline bool skin_influence_ok(int count) {
    return count >= 1 && count <= 4;
}

} // namespace fuse::animation

#pragma once

namespace fuse::animation {

/// True when animation layer count is in 1..8.
[[nodiscard]] inline bool anim_layer_ok(int count) {
    return count >= 1 && count <= 8;
}

} // namespace fuse::animation

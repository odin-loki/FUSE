#pragma once

namespace fuse::scene {

/// True when `count` is in the inclusive 1..500 entity cap.
[[nodiscard]] inline bool entity_cap_ok(int count) {
    return count >= 1 && count <= 500;
}

} // namespace fuse::scene

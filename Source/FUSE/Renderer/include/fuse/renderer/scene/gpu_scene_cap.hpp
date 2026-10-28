#pragma once

namespace fuse::renderer {

/// True when `count` is in the inclusive range 1..1000000.
[[nodiscard]] inline bool gpu_scene_cap_ok(int count) {
    return count >= 1 && count <= 1000000;
}

} // namespace fuse::renderer

#pragma once

namespace fuse::scene {

[[nodiscard]] inline bool svo_depth_ok(int depth) {
    return depth >= 1 && depth <= 12;
}

} // namespace fuse::scene

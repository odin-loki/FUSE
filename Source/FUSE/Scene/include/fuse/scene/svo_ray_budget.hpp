#pragma once

namespace fuse::scene {

/// True when `rays` is a power of two in the closed interval [16, 1024].
[[nodiscard]] inline bool svo_ray_budget_ok(int rays) {
    if (rays < 16 || rays > 1024) {
        return false;
    }
    return (rays & (rays - 1)) == 0;
}

} // namespace fuse::scene

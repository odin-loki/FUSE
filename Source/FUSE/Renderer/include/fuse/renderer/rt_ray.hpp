#pragma once

namespace fuse::renderer {

/// True only for ray counts 1 through 1024 inclusive.
[[nodiscard]] inline bool rt_ray_ok(int rays) {
    return rays >= 1 && rays <= 1024;
}

} // namespace fuse::renderer

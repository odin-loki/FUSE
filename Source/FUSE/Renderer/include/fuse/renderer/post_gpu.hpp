#pragma once

namespace fuse::renderer {

/// True only for passes 0 through 7 inclusive.
[[nodiscard]] inline bool post_gpu_ok(int pass) {
    return pass >= 0 && pass <= 7;
}

} // namespace fuse::renderer

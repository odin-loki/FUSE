#pragma once

namespace fuse::renderer {

/// True only for batch counts 1 through 1024 inclusive.
[[nodiscard]] inline bool cull_batch_ok(int count) {
    return count >= 1 && count <= 1024;
}

} // namespace fuse::renderer

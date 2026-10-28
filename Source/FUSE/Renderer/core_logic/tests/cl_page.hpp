#pragma once

namespace fuse::renderer {

/// True only for page counts 1 through 4096 inclusive.
[[nodiscard]] inline bool cl_page_ok(int count) {
    return count >= 1 && count <= 4096;
}

} // namespace fuse::renderer

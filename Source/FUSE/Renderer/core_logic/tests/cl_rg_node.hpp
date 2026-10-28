#pragma once

namespace fuse::renderer {

/// True only for node counts 1 through 256 inclusive.
[[nodiscard]] inline bool cl_rg_node_ok(int count) {
    return count >= 1 && count <= 256;
}

} // namespace fuse::renderer

#pragma once

namespace fuse::renderer {

/// True only for primitive counts 1 through 128 inclusive.
[[nodiscard]] inline bool meshlet_prim_ok(int count) {
    return count >= 1 && count <= 128;
}

} // namespace fuse::renderer

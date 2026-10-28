#pragma once

// Material layer count: 1 through 8 inclusive.

namespace fuse::renderer {

[[nodiscard]] inline bool material_layer_ok(int count) {
    return count >= 1 && count <= 8;
}

} // namespace fuse::renderer

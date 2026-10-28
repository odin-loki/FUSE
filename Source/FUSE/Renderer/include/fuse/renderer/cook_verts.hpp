#pragma once

namespace fuse::renderer {

/// True only for vertex counts 3 through 256 inclusive.
[[nodiscard]] inline bool cook_verts_ok(int verts) {
    return verts >= 3 && verts <= 256;
}

} // namespace fuse::renderer

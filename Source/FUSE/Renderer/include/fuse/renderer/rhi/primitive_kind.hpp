#pragma once

// CPU check for Vulkan primitive topology. Valid kinds are 0..5
// (point, line, line strip, triangle, triangle strip, triangle fan).

namespace fuse::renderer {

[[nodiscard]] inline bool primitive_kind_ok(int kind) {
    return kind >= 0 && kind <= 5;
}

} // namespace fuse::renderer

#pragma once

// CPU check for Vulkan memory-type indices. Valid range is 0..31.

namespace fuse::renderer {

[[nodiscard]] inline bool memory_type_ok(int index) {
    return index >= 0 && index <= 31;
}

} // namespace fuse::renderer

#pragma once

// CPU check for Vulkan queue family indices. Valid families are 0..3.

namespace fuse::renderer {

[[nodiscard]] inline bool queue_family_ok(int family) {
    return family >= 0 && family <= 3;
}

} // namespace fuse::renderer

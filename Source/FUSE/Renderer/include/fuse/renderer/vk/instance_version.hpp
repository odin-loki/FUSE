#pragma once

// CPU check for Vulkan instance API versions. 1.0 through 1.3 are accepted.

namespace fuse::renderer {

[[nodiscard]] inline bool instance_version_ok(int major, int minor) {
    return major == 1 && minor >= 0 && minor <= 3;
}

} // namespace fuse::renderer

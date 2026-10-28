#pragma once

// CPU check for Vulkan push-constant byte counts. A valid count is a multiple of 4 in 0..128.

namespace fuse::renderer {

[[nodiscard]] inline bool push_bytes_ok(int bytes) {
    return bytes >= 0 && bytes <= 128 && (bytes % 4) == 0;
}

} // namespace fuse::renderer

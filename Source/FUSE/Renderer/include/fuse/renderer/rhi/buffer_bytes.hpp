#pragma once

// CPU check for Vulkan buffer sizes. Valid range is 1..(1<<28) bytes.

#include <cstdint>

namespace fuse::renderer {

[[nodiscard]] inline bool buffer_bytes_ok(std::uint64_t bytes) {
    return bytes >= 1ull && bytes <= (1ull << 28);
}

} // namespace fuse::renderer

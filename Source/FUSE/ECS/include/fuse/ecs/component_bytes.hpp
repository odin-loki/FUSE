#pragma once

#include <cstddef>

namespace fuse::ecs {

/// Component storage size in bytes. Valid when `bytes` is in 1..4096 inclusive.
[[nodiscard]] inline bool component_bytes_ok(std::size_t bytes) {
    return bytes >= 1 && bytes <= 4096;
}

} // namespace fuse::ecs

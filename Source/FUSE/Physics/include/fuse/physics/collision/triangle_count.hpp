#pragma once

#include <cstdint>

namespace fuse::physics {

/// True when a triangle-mesh triangle count is in the inclusive range 1..(1<<20).
[[nodiscard]] inline bool triangle_count_ok(std::uint32_t count) {
    return count >= 1u && count <= (1u << 20);
}

} // namespace fuse::physics

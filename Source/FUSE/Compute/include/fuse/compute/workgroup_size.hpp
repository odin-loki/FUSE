#pragma once

#include <cstdint>

namespace fuse::compute {

/// True when `size` is a power of two in the closed interval [32, 256].
[[nodiscard]] inline bool workgroup_size_ok(std::uint32_t size) {
    if (size < 32u || size > 256u) {
        return false;
    }
    return (size & (size - 1u)) == 0u;
}

} // namespace fuse::compute

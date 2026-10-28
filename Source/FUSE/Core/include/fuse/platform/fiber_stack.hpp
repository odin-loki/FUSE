#pragma once

#include <cstddef>

namespace fuse::platform {

/// True when `bytes` is a power of two in [16384, 1048576].
[[nodiscard]] inline bool fiber_stack_ok(std::size_t bytes) {
    if (bytes < 16384 || bytes > 1048576) {
        return false;
    }
    return (bytes & (bytes - 1)) == 0;
}

} // namespace fuse::platform

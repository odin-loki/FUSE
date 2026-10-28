#pragma once

#include <cstdint>

namespace fuse::memory {

/// True when `bytes` is in [4096, 268435456].
[[nodiscard]] inline bool heap_size_ok(std::uint64_t bytes) {
    return bytes >= 4096u && bytes <= 268435456u;
}

} // namespace fuse::memory

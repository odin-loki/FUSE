#pragma once

#include <cstdint>

namespace fuse::script {

/// True when `usedBytes` is within `limitBytes` (`usedBytes` <= `limitBytes`).
/// A limit of 0 is true only when `usedBytes` is 0.
[[nodiscard]] inline bool memory_within(std::uint32_t usedBytes, std::uint32_t limitBytes) {
    return usedBytes <= limitBytes;
}

} // namespace fuse::script

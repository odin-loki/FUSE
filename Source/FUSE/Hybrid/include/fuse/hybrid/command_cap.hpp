#pragma once

#include <cstdint>

namespace fuse::hybrid {

/// True when `cap` is positive and `count` does not exceed it.
[[nodiscard]] inline bool command_count_ok(std::uint32_t count, std::uint32_t cap) {
    return cap > 0u && count <= cap;
}

} // namespace fuse::hybrid

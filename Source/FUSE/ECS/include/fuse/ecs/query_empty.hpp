#pragma once

#include <cstdint>

namespace fuse::ecs {

/// True only when `mask` is 0.
[[nodiscard]] inline bool query_is_empty(std::uint64_t mask) {
    return mask == 0ull;
}

} // namespace fuse::ecs

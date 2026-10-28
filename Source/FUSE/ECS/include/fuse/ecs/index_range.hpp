#pragma once

#include <cstdint>

namespace fuse::ecs {

/// True when `index` is strictly less than `capacity`. A zero capacity rejects every index.
[[nodiscard]] inline bool index_in_capacity(std::uint32_t index, std::uint32_t capacity) {
    return index < capacity;
}

} // namespace fuse::ecs

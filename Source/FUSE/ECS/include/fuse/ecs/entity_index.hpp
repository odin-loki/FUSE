#pragma once

#include <cstdint>

namespace fuse::ecs {

/// Entity slot index. Valid when `index` is strictly below 1<<20.
[[nodiscard]] inline bool entity_index_ok(std::uint32_t index) {
    return index < (1u << 20);
}

} // namespace fuse::ecs

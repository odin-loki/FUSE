#pragma once

#include <cstdint>

namespace fuse::ecs {

/// True when `has` contains every bit in `allOf` and none of the bits in `noneOf`.
[[nodiscard]] inline bool query_matches(std::uint32_t has, std::uint32_t allOf, std::uint32_t noneOf) {
    return (has & allOf) == allOf && (has & noneOf) == 0;
}

} // namespace fuse::ecs

#pragma once

#include <cstdint>

namespace fuse::ecs {

/// True when `generation` is non-zero. Generation 0 is the dead sentinel.
[[nodiscard]] inline bool generation_alive(std::uint32_t generation) {
    return generation != 0u;
}

} // namespace fuse::ecs

#pragma once

#include <cstdint>

namespace fuse::project {

/// True for cook generations 0 through 1000000 inclusive.
[[nodiscard]] inline bool cook_gen_ok(std::uint32_t generation) {
    return generation <= 1000000u;
}

} // namespace fuse::project

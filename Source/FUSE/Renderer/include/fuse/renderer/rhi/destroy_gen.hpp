#pragma once

// CPU check for resource destroy generations. Valid range is 0..1000000.

#include <cstdint>

namespace fuse::renderer {

[[nodiscard]] inline bool destroy_gen_ok(std::uint32_t generation) {
    return generation <= 1000000u;
}

} // namespace fuse::renderer

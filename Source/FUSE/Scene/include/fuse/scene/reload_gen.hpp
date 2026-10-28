#pragma once

#include <cstdint>

namespace fuse::scene {

[[nodiscard]] inline bool reload_gen_ok(std::uint32_t generation) {
    return generation <= 1000u;
}

} // namespace fuse::scene

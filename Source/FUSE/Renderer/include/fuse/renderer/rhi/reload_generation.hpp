#pragma once

// CPU check for hot-reload generation counters. Valid range is 0..1000000.

#include <cstdint>

namespace fuse::renderer {

[[nodiscard]] inline bool reload_generation_ok(std::uint32_t generation) {
    return generation <= 1000000u;
}

} // namespace fuse::renderer

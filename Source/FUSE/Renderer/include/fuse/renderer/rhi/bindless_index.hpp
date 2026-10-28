#pragma once

// CPU check for bindless descriptor indices. A valid index is below 1<<20.

#include <cstdint>

namespace fuse::renderer {

[[nodiscard]] inline bool bindless_index_ok(std::uint32_t index) {
    return index < (1u << 20);
}

} // namespace fuse::renderer

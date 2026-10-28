#pragma once

#include <cstdint>

namespace fuse::jobs {

/// True when `count` is in the closed range [1, 1<<20].
[[nodiscard]] inline bool parallel_count_ok(std::uint32_t count) {
    return count >= 1u && count <= (1u << 20);
}

} // namespace fuse::jobs

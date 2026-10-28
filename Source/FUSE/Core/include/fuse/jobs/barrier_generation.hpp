#pragma once

#include <cstdint>

namespace fuse::jobs {

/// True when `generation` is in [1, 1024].
[[nodiscard]] inline bool barrier_generation_ok(std::uint64_t generation) {
    return generation >= 1ull && generation <= 1024ull;
}

} // namespace fuse::jobs

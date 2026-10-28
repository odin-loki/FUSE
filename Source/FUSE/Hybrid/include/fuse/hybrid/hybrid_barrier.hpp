#pragma once

namespace fuse::hybrid {

/// True only for generations 0 through 3 inclusive.
[[nodiscard]] inline bool hybrid_barrier_ok(int generation) {
    return generation >= 0 && generation <= 3;
}

} // namespace fuse::hybrid

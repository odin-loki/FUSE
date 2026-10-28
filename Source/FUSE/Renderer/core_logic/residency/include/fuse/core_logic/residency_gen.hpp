#pragma once

namespace fuse::renderer {

/// True only for generations 0 through 1000000 inclusive.
[[nodiscard]] inline bool residency_gen_ok(int generation) {
    return generation >= 0 && generation <= 1000000;
}

} // namespace fuse::renderer

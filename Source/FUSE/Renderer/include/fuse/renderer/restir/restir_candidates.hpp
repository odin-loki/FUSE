#pragma once

namespace fuse::renderer {

/// True when `count` is in the inclusive range 1..32.
[[nodiscard]] inline bool restir_candidates_ok(int count) {
    return count >= 1 && count <= 32;
}

} // namespace fuse::renderer

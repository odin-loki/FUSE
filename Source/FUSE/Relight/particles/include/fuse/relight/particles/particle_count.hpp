#pragma once

namespace fuse::relight {

/// True when `count` is in the inclusive range 0..100000.
[[nodiscard]] inline bool relight_particle_count_ok(int count) {
    return count >= 0 && count <= 100000;
}

} // namespace fuse::relight

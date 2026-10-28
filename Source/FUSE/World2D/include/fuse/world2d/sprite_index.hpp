#pragma once

namespace fuse::world2d {

/// True when `index` is in the closed interval [0, 1023].
[[nodiscard]] inline bool sprite_index_ok(int index) {
    return index >= 0 && index <= 1023;
}

} // namespace fuse::world2d

#pragma once

namespace fuse::renderer {

/// True when `count` is a TAA sample count (2, 4, or 8) and `index` is in `0 .. count - 1`.
[[nodiscard]] inline bool taa_sample_ok(int index, int count) {
    if (count != 2 && count != 4 && count != 8) {
        return false;
    }
    return index >= 0 && index < count;
}

} // namespace fuse::renderer

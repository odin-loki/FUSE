// Direct-illumination candidate count. A count is valid only when it lies in [1, 32].
#pragma once

namespace fuse::relight {

/// True when `count` lies in [1, 32].
[[nodiscard]] inline bool di_candidates_ok(int count) {
    return count >= 1 && count <= 32;
}

} // namespace fuse::relight

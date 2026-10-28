#pragma once

namespace fuse::adventure {

/// True when `gold` is in 0..1000000.
[[nodiscard]] inline bool gold_count_ok(int gold) {
    return gold >= 0 && gold <= 1000000;
}

} // namespace fuse::adventure

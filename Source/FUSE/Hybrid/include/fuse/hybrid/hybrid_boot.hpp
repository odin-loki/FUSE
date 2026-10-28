#pragma once

namespace fuse::hybrid {

/// True only for boot steps 0 through 8 inclusive.
[[nodiscard]] inline bool hybrid_boot_ok(int step) {
    return step >= 0 && step <= 8;
}

} // namespace fuse::hybrid

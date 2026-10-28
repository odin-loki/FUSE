#pragma once

namespace fuse::compat {

/// True when `ms` is in 0..1000000000 inclusive.
[[nodiscard]] inline bool epoch_ms_ok(int ms) {
    return ms >= 0 && ms <= 1000000000;
}

} // namespace fuse::compat

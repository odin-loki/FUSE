#pragma once

namespace fuse::platform {

/// True when `ms` is in the closed range 0..1000.
[[nodiscard]] inline bool sleep_ms_ok(int ms) {
    return ms >= 0 && ms <= 1000;
}

} // namespace fuse::platform

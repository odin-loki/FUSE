#pragma once

namespace fuse::platform {

/// True when `ms` is in the closed range 1..100.
[[nodiscard]] inline bool poll_ms_ok(int ms) {
    return ms >= 1 && ms <= 100;
}

} // namespace fuse::platform

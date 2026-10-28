#pragma once

namespace fuse::platform {

/// True when `ms` is in the closed range 0..2000.
[[nodiscard]] inline bool repeat_delay_ok(int ms) {
    return ms >= 0 && ms <= 2000;
}

} // namespace fuse::platform

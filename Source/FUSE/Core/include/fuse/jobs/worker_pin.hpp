#pragma once

namespace fuse::jobs {

/// True when `pin` is in the closed range [0, 63].
[[nodiscard]] inline bool worker_pin_ok(int pin) {
    return pin >= 0 && pin <= 63;
}

} // namespace fuse::jobs

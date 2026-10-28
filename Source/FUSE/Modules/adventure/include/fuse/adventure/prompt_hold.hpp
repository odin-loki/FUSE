#pragma once

namespace fuse::adventure {

/// True only for hold times from 0 through 2000 milliseconds, inclusive.
[[nodiscard]] inline bool prompt_hold_ok(int ms) {
    return ms >= 0 && ms <= 2000;
}

} // namespace fuse::adventure

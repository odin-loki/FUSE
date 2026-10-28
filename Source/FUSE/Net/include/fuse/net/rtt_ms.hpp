#pragma once

namespace fuse::net {

/// True for millisecond counts in the closed range 0..1000.
[[nodiscard]] inline bool rtt_ms_ok(int milliseconds) noexcept {
    return milliseconds >= 0 && milliseconds <= 1000;
}

} // namespace fuse::net

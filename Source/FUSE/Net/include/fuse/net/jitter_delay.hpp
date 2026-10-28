#pragma once

namespace fuse::net {

/// One-way delay `rtt + jitter` when `rtt` is in 0..1000 and `jitter` is in 0..200.
/// Any other pair returns -1.
[[nodiscard]] inline int jitter_delay(int rtt, int jitter) noexcept {
    if (rtt < 0 || rtt > 1000 || jitter < 0 || jitter > 200) {
        return -1;
    }
    return rtt + jitter;
}

} // namespace fuse::net

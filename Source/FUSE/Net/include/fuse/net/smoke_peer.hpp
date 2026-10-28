#pragma once

namespace fuse::net {

/// True for peer counts in `1..32`.
[[nodiscard]] inline bool smoke_peer_ok(int peers) noexcept {
    return peers >= 1 && peers <= 32;
}

} // namespace fuse::net

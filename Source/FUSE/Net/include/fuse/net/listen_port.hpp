#pragma once

#include <cstdint>

namespace fuse::net {

/// True for listen ports in `1..65535`. Port 0 is not a valid listen port.
[[nodiscard]] inline bool listen_port_ok(std::uint16_t port) noexcept {
    return port != 0;
}

} // namespace fuse::net

#pragma once

#include <cstdint>

namespace fuse::net {

/// Frames the client lags the server (0 when the client is at or ahead of the server).
[[nodiscard]] inline std::uint32_t frames_behind(std::uint32_t serverFrame, std::uint32_t clientFrame) noexcept {
    if (serverFrame < clientFrame) {
        return 0;
    }
    return serverFrame - clientFrame;
}

} // namespace fuse::net

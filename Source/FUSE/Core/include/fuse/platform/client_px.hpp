#pragma once

namespace fuse::platform {

/// True when `px` is in the closed range 1..8192.
[[nodiscard]] inline bool client_px_ok(int px) {
    return px >= 1 && px <= 8192;
}

} // namespace fuse::platform

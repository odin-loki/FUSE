#pragma once

namespace fuse::platform {

/// True when `major` is 1 and `minor` is in the closed range 0..3.
[[nodiscard]] inline bool mobile_api_ok(int major, int minor) {
    return major == 1 && minor >= 0 && minor <= 3;
}

} // namespace fuse::platform

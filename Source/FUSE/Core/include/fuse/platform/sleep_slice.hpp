#pragma once

namespace fuse::platform {

[[nodiscard]] inline bool sleep_slice_ok(int ms) {
    return ms >= 0 && ms <= 16;
}

} // namespace fuse::platform

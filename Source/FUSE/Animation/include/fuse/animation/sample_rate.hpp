#pragma once

namespace fuse::animation {

[[nodiscard]] inline bool sample_rate_ok(int hz) {
    return hz == 30 || hz == 60;
}

} // namespace fuse::animation

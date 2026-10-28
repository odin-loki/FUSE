#pragma once

namespace fuse::log {

/// True when `cap` >= 0, `hits` >= 0, and `hits` <= `cap`.
[[nodiscard]] inline bool rate_allow(int hits, int cap) {
    return cap >= 0 && hits >= 0 && hits <= cap;
}

} // namespace fuse::log

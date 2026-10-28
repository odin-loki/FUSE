#pragma once

namespace fuse::relight {

/// True when \p channel is in 0..15.
[[nodiscard]] inline bool tap_channel_ok(int channel) {
    return channel >= 0 && channel <= 15;
}

} // namespace fuse::relight

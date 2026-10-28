#pragma once

namespace fuse::audio {

/// True when \p channels is a supported output width: mono, stereo, 5.1, or 7.1.
[[nodiscard]] inline constexpr bool output_channel_ok(int channels) {
    return channels == 1 || channels == 2 || channels == 6 || channels == 8;
}

} // namespace fuse::audio

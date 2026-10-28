#pragma once

namespace fuse::audio {

/// True for the supported mix rates: 44100 Hz and 48000 Hz.
[[nodiscard]] inline bool mix_rate_ok(int hz) {
    return hz == 44100 || hz == 48000;
}

} // namespace fuse::audio
